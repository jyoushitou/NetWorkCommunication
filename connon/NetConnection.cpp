#include "NetConnection.h"

namespace Net
{
    // 64位网络字节序与主机字节序转换（跨平台通用实现）
    static uint64_t htonll(uint64_t value)
    {
        return ((uint64_t)htonl(static_cast<uint32_t>(value & 0xFFFFFFFF)) << 32) |
               htonl(static_cast<uint32_t>(value >> 32));
    }

    static uint64_t ntohll(uint64_t value)
    {
        return htonll(value); // 对称操作
    }

    // 构造函数（只有长度）
    MsgNode::MsgNode(int max_len, int serviceID) : MsgNode(-1ULL, max_len, serviceID)
    {
    }

    // 构造函数（有消息Id和长度）
    MsgNode::MsgNode(unsigned long long msg_id_, int max_len, int serviceID)
        : buf(nullptr), total_len(0), cur_len(0), msg_id(msg_id_)
    {
        // 防御性检查：max_len 必须为正数
        if (max_len <= 0)
        {
            Utils::Out_Err("MsgNode: max_len 必须大于 0", serviceID);
            return;
        }

        // 计算出缓存空间
        total_len = max_len;
        // 申请缓存
        buf = new char[total_len + 1];
        // 给最后一个空间为'\0'避免超出空间
        buf[total_len] = '\0';
    }
    // 获取缓冲区指针
    char* MsgNode::GetBuf() const
    {
        return buf;
    }
    // 获取缓冲区总长度
    int MsgNode::GetTotalLen() const
    {
        return total_len;
    }
    // 获取当前读取位置
    int MsgNode::GetCurLen() const
    {
        return cur_len;
    }

    // 获取消息ID
    unsigned long long MsgNode::GetID() const
    {
        return msg_id;
    }

    // 设置当前读取位置
    void MsgNode::SetCurLen(int len)
    {
        cur_len = len;
    }

    // 设置消息ID
    void MsgNode::SetID(unsigned long long msg_id_)
    {
        msg_id = msg_id_;
    }

    // 析构删除缓存
    MsgNode::~MsgNode()
    {
        delete[] buf;
    }

    // 清空缓存
    void MsgNode::Clear()
    {
        // 给所有内容赋值'\0'
        std::memset(buf, '\0', total_len);
        // 将读取指针复位
        cur_len = 0;
    }

    // 接收长度ID节点
    RecvNode::RecvNode(unsigned long long msg_id, int max_len, int serviceID) : MsgNode(msg_id, max_len, serviceID)
    {
    }

    // 接收长度节点
    RecvNode::RecvNode(int max_len, int serviceID) : MsgNode(max_len, serviceID)
    {
    }

    // 发送节点
    SendNode::SendNode(unsigned long long msg_id_, int max_len, int serviceID) : MsgNode(msg_id_, max_len, serviceID)
    {
    }

    // 唯一构造函数
    Connection::Connection(boost::asio::ip::tcp::socket socket, int serviceID_)
        : sock(std::move(socket)), serviceID(serviceID_), sending(false), closing(false)
    {
    }

    // 开始
    void Connection::Start()
    {
        ReadHead();
    }

    // socket关闭
    void Connection::ActuallyClose()
    {
        Utils::Out_Msg("正在关闭socket", serviceID);
        boost::system::error_code ec;
        sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        sock.close(ec);
        Utils::Out_Msg("socket，正在清空队列完成", serviceID);
        send_queue.clear();
        // 防止多次通知关闭
        if (!close_notified)
        {
            // 通知设为真
            close_notified = true;

            // 回调函数
            ToClosed();
        }
    }

    // 关闭函数
    // API 允许外部线程调用
    void Connection::Close()
    {
        Utils::Out_Msg("正在关闭Session", serviceID);

        // 保活
        auto self = shared_from_this();

        boost::asio::post(sock.get_executor(),
                          [this, self]()
                          {
                              if (closing)
                                  return;
                              closing = true;
                              if (!sending && !send_queue.empty())
                                  DoSend();
                              else if (!sending && send_queue.empty())
                                  ActuallyClose();
                          });
    }

    // 读消息体头部
    void Connection::ReadHead()
    {
        Utils::Out_Msg("等待读取消息初始化", serviceID);
        // 保活
        auto self = shared_from_this();

        // 申请缓存
        recv_node = std::make_shared<RecvNode>(HEAD_LENGTH, serviceID);
        // 初始化缓存
        recv_node->Clear();

        Utils::Out_Msg("初始化完成等待数据", serviceID);

        // 读取数据
        boost::asio::async_read(sock, boost::asio::buffer(recv_node->GetBuf(), recv_node->GetTotalLen()),
                                [this, self](boost::system::error_code ec, std::size_t)
                                {
                                    if (ec)
                                    {
                                        Utils::Out_Err(ec.what(), serviceID);
                                        ActuallyClose();
                                        return;
                                    }

                                    Utils::Out_Msg("收到数据！", serviceID);

                                    Utils::Out_Msg("解析数据并检查是否正确ing...", serviceID);

                                    // 读取的消息长度（4字节）
                                    uint32_t msg_len = 0;
                                    // 读取的消息ID（8字节）
                                    uint64_t msg_id = 0;

                                    // 获取msg_id（读ID）
                                    std::memcpy(&msg_id, recv_node->GetBuf(), HEAD_ID_LENGTH);
                                    // 获取msg_len（读长度）
                                    std::memcpy(&msg_len, recv_node->GetBuf() + HEAD_ID_LENGTH, HEAD_LEN_LENGTH);

                                    // 网络字节序转换成本地字节序（长度用32位转换，ID用64位转换）
                                    msg_len = ntohl(msg_len);
                                    msg_id = ntohll(msg_id);

                                    // 判断传入数据是否正确
                                    if (msg_len > MAX_LENGTH || msg_len <= 0)
                                    {
                                        Utils::Out_Err("收到的消息的长度错误，请修复后重连", serviceID);
                                        Close();
                                        return;
                                    }

                                    Utils::Out_Msg("解析完成！ID：" + std::to_string(msg_id) + "，长度：" +
                                                       std::to_string(msg_len),
                                                   serviceID);
                                    if (!closing)
                                    {
                                        Utils::Out_Net_Msg(msg_id, "准备读取具体消息", serviceID);
                                        // 读取消息体
                                        ReadBody(msg_id, static_cast<int>(msg_len));
                                    }
                                    else
                                    {
                                        Utils::Out_Msg("正处在关闭连接,拒绝接收新消息", serviceID);
                                        // 关闭连接
                                        ActuallyClose();
                                    }
                                });
    }

    // 读取消息体
    void Connection::ReadBody(unsigned long long msg_id, int msg_len)
    {
        Utils::Out_Msg("检查接收状态", serviceID);
        // 检查是否在关闭状态
        if (closing)
        {
            Utils::Out_Err("收到消息，但是正在关闭连接，拒绝接收消息", serviceID);
            ActuallyClose();
            return;
        }

        Utils::Out_Msg("接收状态正确，开始接收", serviceID);

        // 保活
        auto self = shared_from_this();

        // 申请接收缓存
        recv_node = std::make_shared<RecvNode>(msg_id, msg_len, serviceID);
        // 清理缓存
        recv_node->Clear();

        // 接收消息
        boost::asio::async_read(sock, boost::asio::buffer(recv_node->GetBuf(), recv_node->GetTotalLen()),
                                [this, self, msg_id](boost::system::error_code ec, std::size_t)
                                {
                                    // 判断是否有异常
                                    if (ec)
                                    {
                                        Utils::Out_Err("出现错误：" + ec.what(), serviceID);
                                        // 关闭连接
                                        ActuallyClose();
                                        return;
                                    }

                                    Utils::Out_Msg("解析消息中", serviceID);

                                    // 设置目标长度
                                    recv_node->SetCurLen(recv_node->GetTotalLen());

                                    // 消息装换为string类型
                                    std::string msg(recv_node->GetBuf(), recv_node->GetCurLen());

                                    Utils::Out_Msg("解析完成，开始尝试将消息抛出", serviceID);
                                    // 尝试输出消息
                                    try
                                    {
                                        ToWork(msg_id, msg);
                                    }
                                    // 捕获异常
                                    catch (const std::exception& e)
                                    {
                                        Utils::Out_Err(std::string("抛出异常: ") + e.what(), serviceID);
                                        // 关闭连接
                                        Close();
                                    }
                                    catch (...)
                                    {
                                        Utils::Out_Err("未知异常", serviceID);
                                        Close();
                                    }
                                    // 如果现在socket连接并且不在关闭状态
                                    if (sock.is_open() && !closing)
                                    {
                                        // 继续等待读取头文件
                                        ReadHead();
                                    }
                                });
    }

    void Connection::ToSend(const std::string& msg)
    {
        // 原子自增，线程安全
        ToSend(g_net_msg_id.fetch_add(1, std::memory_order_relaxed), msg);
    }

    // 外部发送函数
    void Connection::ToSend(unsigned long long msg_id, const std::string& msg)
    {
        // 加入发送队列
        Send(msg_id, msg);
    }

    // 发送函数队列
    void Connection::Send(unsigned long long msg_id, std::string msg)
    {
        // 保活
        auto self = shared_from_this();

        Utils::Out_Net_Msg(msg_id, "正在构建发送队列", serviceID);

        // 获得其他线程的发送调用
        boost::asio::post(sock.get_executor(),
                          [this, self, msg_id, msg = std::move(msg)]() mutable
                          {
                              // 检查是否在关闭状态
                              if (closing)
                              {
                                  Utils::Out_Err("准备发送消息，但是正在关闭连接，拒绝添加任务到发送队列", serviceID);
                                  return;
                              }
                              // 判断传入消息是否过长
                              if (msg.size() > MAX_LENGTH || msg.size() <= 0)
                              {
                                  Utils::Out_Err("传入消息的长度错误，请修复后重试", serviceID);
                                  return;
                              }

                              // 构建发送任务
                              auto send_node = std::make_shared<SendNode>(
                                  msg_id, HEAD_LENGTH + static_cast<int>(msg.size()), serviceID);
                              // 获取消息缓存
                              char* buf = send_node->GetBuf();

                              // 转换字节序（长度用32位，ID用64位）
                              uint32_t net_msg_len = htonl(static_cast<int>(msg.size()));
                              uint64_t net_msg_id = htonll(static_cast<uint64_t>(msg_id));

                              // 写入缓存（写ID，写长度）
                              std::memcpy(buf, &net_msg_id, HEAD_ID_LENGTH);
                              std::memcpy(buf + HEAD_ID_LENGTH, &net_msg_len, HEAD_LEN_LENGTH);
                              if (!msg.empty())
                              {
                                  std::memcpy(buf + HEAD_LENGTH, msg.data(), static_cast<int>(msg.size()));
                              }
                              // 设置发送长度
                              send_node->SetCurLen(HEAD_LENGTH + static_cast<int>(msg.size()));
                              send_node->SetID(msg_id);

                              // 外层 lambda 已在 IO 线程中执行，直接入队
                              send_queue.push_back(send_node);
                              if (!sending)
                              {
                                  Utils::Out_Net_Msg(msg_id, "消息队列构任务建完成，进入消息队列等待发送", serviceID);

                                  // 启动发送队列
                                  DoSend();
                              }
                          });
    }

    // 发送消息
    void Connection::DoSend()
    {
        Utils::Out_Msg("正在检查发送条件", serviceID);
        // 判断是否有发送的消息
        if (send_queue.empty())
        {
            // 将发送状态变量更新
            sending = false;
            // 队列发完且请求过关闭
            if (closing)
            {
                ActuallyClose();
            }
            return;
        }

        Utils::Out_Msg("检查完毕，准备发送", serviceID);
        // 更新发送状态变量
        sending = true;
        // 获取发送任务
        auto send_node = send_queue.front();
        // 保活
        auto self = shared_from_this();

        Utils::Out_Net_Msg(send_node->GetID(), "正在发送消息", serviceID);
        // 异步发送
        boost::asio::async_write(sock, boost::asio::buffer(send_node->GetBuf(), send_node->GetCurLen()),
                                 [this, self, send_node](boost::system::error_code ec, std::size_t)
                                 {
                                     // 判断是否有错误
                                     if (ec)
                                     {
                                         Utils::Out_Err("发送错误，值为：" + ec.what(), serviceID);
                                         sending = false;
                                         // 发送失败，直接关闭（丢弃剩余队列）
                                         send_queue.clear();
                                         ActuallyClose();
                                         return;
                                     }

                                     // 弹出发送队列
                                     send_queue.pop_front();

                                     Utils::Out_Msg("检查发送队列是否有发送任务", serviceID);
                                     // 判断队列是否为空
                                     if (!send_queue.empty())
                                     {
                                         Utils::Out_Msg("发送队列有发送任务，继续发送", serviceID);
                                         // 不为空，继续发送
                                         DoSend();
                                     }
                                     else
                                     {
                                         Utils::Out_Msg("发送队列无发送任务", serviceID);
                                         // 为空更新发送队列变量
                                         sending = false;
                                         //
                                         if (closing)
                                         {
                                             // 关闭连接
                                             ActuallyClose();
                                         }
                                     }
                                 });
    }

    // 基类默认空实现，派生类可按需重写
    void Connection::ToClosed()
    {
    }

    // 基类默认空实现，派生类可根据需要重写
    void Connection::ToWork(unsigned long long, std::string)
    {
        // 默认不处理任何业务逻辑
    }
} // namespace Net