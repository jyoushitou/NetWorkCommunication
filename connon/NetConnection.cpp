#include "NetConnection.h"

namespace Net
{
    // 构造函数（只有长度）
    MsgNode::MsgNode(int max_len, int serviceID) : MsgNode(max_len, -1, serviceID)
    {
    }

    // 构造函数（有消息Id和长度）
    MsgNode::MsgNode(int max_len, int msg_id_, int serviceID) : buf(nullptr), total_len(0), cur_len(0), msg_id(msg_id_)
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
    int MsgNode::GetID() const
    {
        return msg_id;
    }

    // 设置当前读取位置
    void MsgNode::SetCurLen(int len)
    {
        cur_len = len;
    }

    // 设置消息ID
    void MsgNode::SetID(int msg_id_)
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
    RecvNode::RecvNode(int max_len, int msg_id, int serviceID) : MsgNode(max_len, msg_id, serviceID)
    {
    }

    // 接收长度节点
    RecvNode::RecvNode(int max_len, int serviceID) : MsgNode(max_len, serviceID)
    {
    }

    // 发送节点
    SendNode::SendNode(int max_len, int msg_id_, int serviceID) : MsgNode(max_len, msg_id_, serviceID)
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
        boost::system::error_code ec;
        sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        sock.close(ec);
        send_queue.clear();
    }

    // 关闭函数
    // API 允许外部线程调用
    void Connection::Close()
    {
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
        // 保活
        auto self = shared_from_this();

        // 申请缓存
        recv_node = std::make_shared<RecvNode>(HEAD_LENGTH, serviceID);
        // 初始化缓存
        recv_node->Clear();

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

                                    // 读取的消息ID
                                    int msg_id = 0;
                                    // 读取的消息长度
                                    int msg_len = 0;

                                    // 获取msg_id
                                    std::memcpy(&msg_id, recv_node->GetBuf(), HEAD_ID_LENGTH);
                                    // 获取msg_len
                                    std::memcpy(&msg_len, recv_node->GetBuf() + HEAD_ID_LENGTH, HEAD_LEN_LENGTH);

                                    // 网络字节序转换成本地字节序
                                    msg_id = ntohl(msg_id);
                                    msg_len = ntohl(msg_len);

                                    // 判断传入数据是否正确
                                    if (msg_len > MAX_LENGTH || msg_len <= 0)
                                    {
                                        Utils::Out_Err("收到的消息的长度错误，请修复后重连", serviceID);
                                        Close();
                                        return;
                                    }

                                    if (!closing)
                                    {
                                        // 读取消息体
                                        ReadBody(msg_id, msg_len);
                                    }
                                    else
                                    {
                                        Utils::Out_Msg("正在关闭连接,拒绝接收", serviceID);
                                        // 关闭连接
                                        ActuallyClose();
                                    }
                                });
    }

    // 读取消息体
    void Connection::ReadBody(int msg_id, int msg_len)
    {
        // 检查是否在关闭状态
        if (closing)
        {
            Utils::Out_Err("收到消息，但是正在关闭连接，拒绝接收", serviceID);
            ActuallyClose();
            return;
        }

        // 保活
        auto self = shared_from_this();

        // 申请接收缓存
        recv_node = std::make_shared<RecvNode>(msg_len, msg_id, serviceID);
        // 清理缓存
        recv_node->Clear();

        // 接收消息
        boost::asio::async_read(sock, boost::asio::buffer(recv_node->GetBuf(), recv_node->GetTotalLen()),
                                [this, self, msg_id](boost::system::error_code ec, std::size_t)
                                {
                                    if (ec)
                                    {
                                        Utils::Out_Err(ec.what(), serviceID);
                                        ActuallyClose();
                                        return;
                                    }
                                    recv_node->SetCurLen(recv_node->GetTotalLen());
                                    std::string msg(recv_node->GetBuf(), recv_node->GetCurLen());
                                    try
                                    {
                                        ToWork(msg_id, msg);
                                    }
                                    catch (const std::exception& e)
                                    {
                                        Utils::Out_Err(std::string("ToWork 异常: ") + e.what(), serviceID);
                                        Close();
                                    }
                                    catch (...)
                                    {
                                        Utils::Out_Err("ToWork 未知异常", serviceID);
                                        Close();
                                    }
                                    if (sock.is_open() && !closing)
                                    {
                                        ReadHead();
                                    }
                                });
    }

    // 发送函数
    void Connection::Send(int msg_id, std::string msg)
    {
        // 保活
        auto self = shared_from_this();

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
                              if (msg.size() > MAX_LENGTH)
                              {
                                  Utils::Out_Err("传入消息的长度错误，请修复后重试", serviceID);
                                  return;
                              }
                              // 构建发送任务
                              auto send_node = std::make_shared<SendNode>(HEAD_LENGTH + msg.size(), msg_id, serviceID);
                              // 获取消息缓存
                              char* buf = send_node->GetBuf();

                              // 转换字节序
                              int32_t net_msg_id = htonl(msg_id);
                              int32_t net_msg_len = htonl(static_cast<int>(msg.size()));

                              // 写入缓存
                              std::memcpy(buf, &net_msg_id, HEAD_ID_LENGTH);
                              std::memcpy(buf + HEAD_ID_LENGTH, &net_msg_len, HEAD_LEN_LENGTH);
                              if (!msg.empty())
                              {
                                  std::memcpy(buf + HEAD_LENGTH, msg.data(), static_cast<int>(msg.size()));
                              }
                              // 设置发送长度
                              send_node->SetCurLen(HEAD_LENGTH + static_cast<int>(msg.size()));

                              // 外层 lambda 已在 IO 线程中执行，直接入队
                              send_queue.push_back(send_node);
                              if (!sending)
                              {
                                  DoSend();
                              }
                          });
    }

    // 发送消息
    void Connection::DoSend()
    {
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

        // 更新发送状态变量
        sending = true;
        // 获取发送任务
        auto send_node = send_queue.front();
        // 保活
        auto self = shared_from_this();

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

                                     // 判断队列是否为空
                                     if (!send_queue.empty())
                                     {
                                         // 不为空，继续发送
                                         DoSend();
                                     }
                                     else
                                     {
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

    // 基类默认空实现，派生类可根据需要重写
    void Connection::ToWork(int, std::string)
    {
        // 默认不处理任何业务逻辑
    }
} // namespace Net