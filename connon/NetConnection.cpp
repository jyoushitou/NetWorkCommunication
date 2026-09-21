/// @file        NetConnection.cpp
/// @brief       网络通讯连接核心实现（消息缓存、读写、发送队列）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// 头文件
#include "NetConnection.h"

#include <cstring>

#include <boost/asio.hpp>

#include "Message.h"

extern int Service_ID;

namespace Net
{
    /// @brief      64位主机字节序转网络字节序
    /// @details    跨平台通用实现
    /// @param[in] value 待转换的64位主机字节序数值
    /// @return     转换后的64位网络字节序数值
    /// @note       与 ntohll 互为对称操作
    static uint64_t htonll(uint64_t value)
    {
        return ((uint64_t)htonl(static_cast<uint32_t>(value & 0xFFFFFFFF)) << 32) |
               htonl(static_cast<uint32_t>(value >> 32));
    }

    /// @brief      64位网络字节序转主机字节序
    /// @details    与 htonll 对称
    /// @param[in] value 待转换的64位网络字节序数值
    /// @return     转换后的64位主机字节序数值
    /// @note
    static uint64_t ntohll(uint64_t value)
    {
        return htonll(value); // 对称操作
    }

    /// @brief      构造函数（只有长度）
    /// @details    仅指定缓存长度，消息ID默认为无效值
    /// @param[in] max_len 缓存的最大长度
    /// @note
    MsgNode::MsgNode(int max_len) : MsgNode(-1ULL, max_len)
    {
    }

    /// @brief      构造函数（有消息ID和长度）
    /// @details    指定消息ID与缓存长度，并按长度申请缓冲区
    /// @param[in] msg_id_ 消息全局唯一ID
    /// @param[in] max_len 缓存的最大长度
    /// @warning    禁止传非正数，否则缓冲区申请失败
    /// @note
    MsgNode::MsgNode(unsigned long long msg_id, int max_len)
    {
        // 将buf指针置空
        buf = nullptr;
        // 设置目标值
        total_len = 0;
        // 设置当前值
        cur_len = 0;
        // 设置消息ID
        this->msg_id = msg_id;

        // 防御性检查：max_len 必须为正数
        if (max_len <= 0)
        {
            Utils::Out::outErr("MsgNode: max_len 必须大于 0");
            return;
        }

        // 计算出缓存空间
        total_len = max_len;
        // 申请缓存
        buf = new char[total_len + 1];
        // 给最后一个空间为'\0'避免超出空间
        buf[total_len] = '\0';
    }
    /// @brief      获取缓存区指针
    /// @return     指向缓冲区首地址的指针
    /// @note
    char* MsgNode::getBuf() const
    {
        return buf;
    }
    /// @brief      获取缓存区总长度
    /// @return     缓冲区总长度
    /// @note
    int MsgNode::getTotalLen() const
    {
        return total_len;
    }
    /// @brief      获取当前读取位置
    /// @return     当前读取位置
    /// @note
    int MsgNode::getCurLen() const
    {
        return cur_len;
    }

    /// @brief      获取消息ID
    /// @return     消息全局唯一ID
    /// @note
    unsigned long long MsgNode::getID() const
    {
        return msg_id;
    }

    /// @brief      设置当前读取位置
    /// @param[in] len 当前读取到的位置
    /// @note
    void MsgNode::setCurLen(int len)
    {
        cur_len = len;
    }

    /// @brief      设置消息ID
    /// @param[in] msg_id_ 消息全局唯一ID
    /// @note
    void MsgNode::setID(unsigned long long msg_id_)
    {
        msg_id = msg_id_;
    }

    /// @brief      析构函数
    /// @details    释放消息体缓存
    /// @note
    MsgNode::~MsgNode()
    {
        delete[] buf;
    }

    /// @brief      清空缓存
    /// @details    将缓存内容置零并复位读取位置
    /// @note
    void MsgNode::clear()
    {
        // 给所有内容赋值'\0'
        std::memset(buf, '\0', total_len);
        // 将读取指针复位
        cur_len = 0;
    }

    /// @brief      构造函数（有消息ID和长度）
    /// @param[in] msg_id 消息全局唯一ID
    /// @param[in] max_len 缓存的最大长度
    /// @note
    RecvNode::RecvNode(unsigned long long msg_id, int max_len) : MsgNode(msg_id, max_len)
    {
    }

    /// @brief      构造函数（只有长度）
    /// @param[in] max_len 缓存的最大长度
    /// @note
    RecvNode::RecvNode(int max_len) : MsgNode(max_len)
    {
    }

    /// @brief      构造函数
    /// @param[in] msg_id 消息全局唯一ID
    /// @param[in] max_len 缓存的最大长度
    /// @note
    SendNode::SendNode(unsigned long long msg_id, int max_len) : MsgNode(msg_id, max_len)
    {
    }

    /// @brief      构造函数
    /// @details    唯一的构造函数，初始化socket与内部状态
    /// @param[in] socket 连接的socket
    /// @note
    Connection::Connection(boost::asio::ip::tcp::socket socket, boost::asio::io_context& io)
        : socket(std::move(socket)), ioc(io)
    {
        // 发送状态
        // 初始化为false
        sending = false;
        // 关闭状态
        // 初始化为false
        closing = false;
    }

    /// @brief      开始函数
    /// @details    启动连接的异步读取流程
    /// @warning    须在socket连接建立后调用
    /// @note
    void Connection::start()
    {
        // 启动读取函数
        readHead();
    }

    /// @brief      关闭socket
    /// @details    关闭socket并处理发送队列，触发一次关闭回调
    /// @warning    内部使用，禁止外部直接调用
    /// @note
    void Connection::actuallyClose()
    {
        Utils::Out::outMsg("正在关闭socket");

        if (!closing)
        {
            closing = true;
        }

        // 关闭socket时查看错误码
        boost::system::error_code ec;
        socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket.close(ec);

        sendQueue.clear();

        // 防止多次通知关闭
        if (!closeNotified)
        {
            // 通知设为真
            closeNotified = true;
        }
    }

    /// @brief      关闭连接
    /// @details    向 IO 线程投递关闭请求，API 允许外部线程调用
    /// @warning    异步执行，调用后连接不再可用
    /// @note
    void Connection::close()
    {
        Utils::Out::outMsg("正在关闭Session");

        // 保活
        auto self = shared_from_this();

        boost::asio::post(socket.get_executor(),
                          [this, self]()
                          {
                              // 已经标记过在关闭状态
                              if (closing)
                              {
                                  return;
                              }
                              // 标记关闭状态
                              closing = true;

                              // 判断是否发送完毕或者在发送状态
                              // 是：则发送消息
                              if (!sending && sendQueue.empty())
                              {
                                  // 启动关闭函数
                                  actuallyClose();
                              }
                              // 不在发送状态且没有消息，直接关闭Connection
                              else
                              {
                                  doSend();
                              }
                          });
    }

    /// @brief      读取头部
    /// @details    异步读取消息头部（ID与长度），校验后进入消息体读取
    /// @warning    解析失败或长度非法会关闭连接
    /// @note
    void Connection::readHead()
    {
        // 保活
        auto self = shared_from_this();

        // 申请缓存
        recvNode = std::make_shared<RecvNode>(HEAD_LENGTH);
        recvNode->clear();

        // 读取数据
        boost::asio::async_read(socket, boost::asio::buffer(recvNode->getBuf(), recvNode->getTotalLen()),
                                [this, self](boost::system::error_code ec, std::size_t)
                                {
                                    // 看看是否有错误
                                    if (ec)
                                    {
                                        Utils::Out::outErr(ec.what());
                                        actuallyClose();
                                        return;
                                    }

                                    Utils::Out::outMsg("收到数据！解析数据并检查是否正确ing");

                                    // 读取的消息长度（4字节）
                                    uint32_t msg_len = 0;
                                    // 读取的消息ID（8字节）
                                    uint64_t msg_id = 0;

                                    // 获取msg_id（读ID）
                                    std::memcpy(&msg_id, recvNode->getBuf(), HEAD_ID_LENGTH);
                                    // 获取msg_len（读长度）
                                    std::memcpy(&msg_len, recvNode->getBuf() + HEAD_ID_LENGTH, HEAD_LEN_LENGTH);

                                    // 网络字节序转换成本地字节序（长度用32位转换，ID用64位转换）
                                    msg_len = ntohl(msg_len);
                                    msg_id = ntohll(msg_id);

                                    // 判断传入数据是否正确
                                    if (msg_len > MAX_LENGTH || msg_len <= 0)
                                    {
                                        Utils::Out::outErr("收到的消息的长度错误，请修复后重连");
                                        close();
                                        return;
                                    }

                                    Utils::Out::outMsg("解析完成！ID：" + std::to_string(msg_id) + "，长度：" +
                                                       std::to_string(msg_len));
                                    // 判断是否处于关闭状态
                                    if (!closing)
                                    {
                                        // 读取消息体
                                        readBody(msg_id, static_cast<int>(msg_len));
                                    }
                                    else
                                    {
                                        Utils::Out::outMsg("正处在关闭连接,拒绝接收新消息");

                                        // 关闭连接
                                        actuallyClose();
                                    }
                                });
    }

    /// @brief      读取消息体
    /// @details    异步读取消息体内容，读取完成后回调业务处理函数
    /// @param[in] msg_id 消息全局唯一ID
    /// @param[in] msg_len 消息体长度
    /// @warning    业务回调抛出的异常会被捕获并关闭连接
    /// @note
    void Connection::readBody(unsigned long long msg_id, int msg_len)
    {
        // 检查是否在关闭状态
        if (closing)
        {
            Utils::Out::outErr("收到消息，但是正在关闭连接，拒绝接收消息");
            actuallyClose();
            return;
        }

        // 保活
        auto self = shared_from_this();

        // 申请接收缓存
        // 使用共享指针
        recvNode = std::make_shared<RecvNode>(msg_id, msg_len);
        // 清理缓存
        recvNode->clear();

        // 接收消息
        boost::asio::async_read(socket, boost::asio::buffer(recvNode->getBuf(), recvNode->getTotalLen()),
                                [this, self, msg_id](boost::system::error_code ec, std::size_t)
                                {
                                    // 判断是否有异常
                                    if (ec)
                                    {
                                        Utils::Out::outErr("出现错误：" + ec.what() + "关闭连接");
                                        // 关闭连接
                                        actuallyClose();
                                        return;
                                    }

                                    // 设置目标长度
                                    recvNode->setCurLen(recvNode->getTotalLen());

                                    // 消息装换为string类型
                                    std::string msg(recvNode->getBuf(), recvNode->getCurLen());

                                    Utils::Out::outMsg("接收完成");

                                    // 更新函数
                                    updateTime();

                                    // 尝试输出消息
                                    try
                                    {
                                        recvToWork(msg_id, msg);
                                    }
                                    // 捕获异常
                                    catch (const std::exception& e)
                                    {
                                        Utils::Out::outErr(std::string("抛出异常: ") + e.what());
                                        // 关闭连接
                                        actuallyClose();
                                    }
                                    catch (...)
                                    {
                                        Utils::Out::outErr("未知异常");
                                        actuallyClose();
                                    }
                                    // 如果现在socket连接并且不在关闭状态
                                    if (socket.is_open() && !closing)
                                    {
                                        // 继续等待下次读取头文件
                                        readHead();
                                    }
                                });
    }

    /// @brief      发送任务创建
    /// @details    外部发送函数，显式指定 msg_id，内部转调 send
    /// @param[in] msg_id 消息全局唯一ID
    /// @param[in] msg 消息序列化字符串
    /// @warning    禁止传入空消息或超长消息，就会抛出异常
    /// @note
    void Connection::toSend(unsigned long long msg_id, const std::string& msg)
    {
        if (msg_id < 0 || msg.size() == 0 || msg.size() > MAX_LENGTH)
        {
            throw std::invalid_argument("msg_id非法或者要发送的消息错误");
        }
        // 加入发送队列
        send(msg_id, msg);
    }

    /// @brief      发送消息到发送队列
    /// @details    把消息封装为发送任务并加入发送队列，线程安全
    /// @param[in] msg_id 消息全局唯一ID
    /// @param[in] msg 消息序列化字符串
    /// @warning    禁止传入空消息或超长消息
    /// @note
    void Connection::send(unsigned long long msg_id, std::string msg)
    {
        // 保活获取自身this指针
        auto self = shared_from_this();

        // 获得其他线程的发送调用
        boost::asio::post(socket.get_executor(),
                          [this, self, msg_id, msg = std::move(msg)]() mutable
                          {
                              // 检查是否在关闭状态
                              if (closing)
                              {
                                  Utils::Out::outErr("准备发送消息，但是正在关闭连接，拒绝添加任务到发送队列");
                                  return;
                              }

                              // 构建发送任务
                              auto send_node =
                                  std::make_shared<SendNode>(msg_id, HEAD_LENGTH + static_cast<int>(msg.size()));

                              // 获取消息缓存空间
                              char* buf = send_node->getBuf();

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
                              send_node->setCurLen(HEAD_LENGTH + static_cast<int>(msg.size()));
                              // 设置发送ID
                              send_node->setID(msg_id);

                              // 外层 lambda 已在 IO 线程中执行，直接入队
                              sendQueue.push_back(send_node);

                              updateTime();

                              // 判断是否在发送状态，不是就启动发送，是则等待
                              if (!sending)
                              {
                                  // 启动发送队列
                                  doSend();
                              }
                          });
    }

    /// @brief      发送消息
    /// @details    从发送队列取出任务并异步发送，IO线程内调用
    /// @warning    发送失败会丢弃剩余队列并关闭连接
    /// @note
    void Connection::doSend()
    {
        // 判断是否有发送的消息
        if (sendQueue.empty())
        {
            // 将发送状态变量更新
            sending = false;
            // 队列发完且未请求过关闭
            if (!closing)
            {
                actuallyClose();
            }
            return;
        }

        // 更新发送状态变量
        sending = true;

        // 获取发送任务
        auto send_node = sendQueue.front();

        // 保活
        auto self = shared_from_this();

        Utils::Out::outNetMsg(send_node->getID(), "发送消息");

        // 异步发送
        boost::asio::async_write(socket, boost::asio::buffer(send_node->getBuf(), send_node->getCurLen()),
                                 [this, self, send_node](boost::system::error_code ec, std::size_t)
                                 {
                                     // 判断是否有错误
                                     if (ec)
                                     {
                                         Utils::Out::outErr("发送错误，值为：" + ec.what());
                                         sending = false;
                                         actuallyClose();
                                         return;
                                     }

                                     // 弹出发送队列
                                     sendQueue.pop_front();

                                     // 判断队列是否为空
                                     if (!sendQueue.empty())
                                     {
                                         // 不为空，继续发送
                                         doSend();
                                     }
                                     else
                                     {
                                         Utils::Out::outMsg("发送完毕");
                                         // 为空更新发送队列变量
                                         sending = false;
                                         // 判断是否为关闭状态
                                         if (closing)
                                         {
                                             // 关闭连接
                                             actuallyClose();
                                         }
                                     }
                                 });
    }
} // namespace Net