/// @file        NetConnection.cpp
/// @brief       网络通讯连接核心实现（消息缓存、读写、发送队列）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// 头文件
#include "NetConnection.h"

#include <cstring>
#include <cstdint>
#include <stdexcept>

#include <boost/asio.hpp>

#include "Utils.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h> // htonl / ntohl
#else
#include <arpa/inet.h> // htonl / ntohl
#endif

namespace Net
{
    /// @brief      64位主机字节序转网络字节序
    /// @details    跨平台通用实现
    /// @param[in] value 待转换的64位主机字节序数值
    /// @return     转换后的64位网络字节序数值
    /// @note       与 ntohll 互为对称操作
    static uint64_t htonll(uint64_t value)
    {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        return value; // 大端：本机序即网络序
#else
        return (static_cast<std::uint64_t>(htonl(static_cast<std::uint32_t>(value & 0xFFFFFFFFu))) << 32) |
               static_cast<std::uint64_t>(htonl(static_cast<std::uint32_t>(value >> 32)));
#endif
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
    /// @details    委托到 MsgNode(msg_id, max_len)，消息ID传 -1ULL 表示无效值
    /// @param[in] max_len 缓存的最大长度
    /// @note
    MsgNode::MsgNode(int max_len) : MsgNode(-1ULL, max_len)
    {
    }

    /// @brief      构造函数（有消息ID和长度）
    /// @details    指定消息ID与缓存长度，并按长度申请缓冲区
    /// @param[in] msg_id 消息全局唯一ID
    /// @param[in] max_len 缓存的最大长度
    /// @warning    禁止传非正数，否则抛出 std::invalid_argument
    /// @note
    MsgNode::MsgNode(unsigned long long msg_id, int max_len)
    {
        // 将buf指针置空
        buf = nullptr;
        // 设置当前值
        cur_len = 0;
        // 设置消息ID
        this->msg_id = msg_id;

        clear(max_len);
    }
    /// @brief      获取缓存区指针
    /// @return     指向缓冲区首地址的指针
    /// @note
    char* MsgNode::getBuf()
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
    /// @param[in] msg_id 消息全局唯一ID
    /// @note
    void MsgNode::setID(unsigned long long msg_id)
    {
        this->msg_id = msg_id;
    }

    /// @brief      析构函数
    /// @details    释放消息体缓存
    /// @note
    MsgNode::~MsgNode()
    {
        delete[] buf;
    }

    /// @brief      清空缓存
    /// @details    将缓存内容整块置零并复位读取位置
    /// @warning    缓冲区为空指针或长度非法时抛出 std::invalid_argument
    /// @note
    void MsgNode::clear()
    {
        // 判断buf是否有指针，且total_len有数据
        if (buf == nullptr)
        {
            throw std::invalid_argument("buf为悬空指针，请重新创建");
        }
        else if (total_len <= 0)
        {
            throw std::invalid_argument("total_len非法，当前total长度为" + std::to_string(total_len));
        }

        std::memset(buf, '\0', static_cast<std::size_t>(total_len));

        // 将读取指针复位
        cur_len = 0;
    }

    /// @brief      重新分配缓存区大小
    /// @details    释放旧缓冲区，按 len 重新申请 total_len + 1 字节并把末位置 '\0'，最后调用 clear() 清零
    /// @param[in] len 新的缓存区长度（字节）
    /// @warning    禁止传非正数，否则抛出 std::invalid_argument
    /// @note       构造函数内部会调用；也可用于复用同一个缓存对象
    void MsgNode::clear(int len)
    {
        // 防御性检查：len 必须为正数
        if (len <= 0)
        {
            throw std::invalid_argument("MsgNode: max_len 必须大于 0 ,当前total_len值为" + std::to_string(total_len));
        }

        if (buf != nullptr)
        {
            // 删除之前的缓存区大小
            delete[] buf;
            // 赋值避免指向野指针
            buf = nullptr;
        }

        // 设置新的缓冲区大小
        total_len = len;

        // 申请新的缓冲区大小
        buf = new char[total_len + 1];

        buf[total_len] = '\0';

        // 清理大小
        clear();
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
    /// @details    唯一的构造函数，接管 socket 并初始化发送/关闭相关的内部状态
    /// @param[in] socket 连接的socket（按值传入，内部 move）
    /// @param[in] io 保存会话的io_context
    /// @param[in] maxSendBufLen 发送队列允许积压的最大字节数
    /// @warning    io 的生命周期必须长于本连接
    /// @note
    Connection::Connection(boost::asio::ip::tcp::socket socket, boost::asio::io_context& io, int maxSendBufLen)
        : socket(std::move(socket)), ioc(io)
    {
        // 发送状态
        // 初始化为false
        sending = false;
        // 关闭状态
        // 初始化为false
        closing = false;

        // 关闭通知标记
        closeNotified.store(false);

        // socket关闭标志
        closeSocket.store(false);

        // 发送队列积压字节数清零
        queueBufLen = 0;

        // 初始化发送队列允许积压的最大字节数
        this->maxSendBufLen = maxSendBufLen;
    }

    /// @brief      开启 TCP_NODELAY
    /// @details    把 socket 的 no_delay 选项置为 true（禁用 Nagle 算法，小包立即发送）；
    ///             设置失败只打印错误日志，不影响正常收发
    /// @warning    须在 start() 之前调用
    /// @note       函数名沿用历史命名（off + NoDelay），实际效果是「开启 NoDelay」
    void Connection::offNoDelay()
    {
        // 开启 TCP_NODELAY（禁用 Nagle 算法，小包立即发送）
        boost::system::error_code ec;
        socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);

        if (ec)
        {
            Utils::Out::outErr("关闭NoDelay时出现错误（只影响性能，不会对于消息传递有影响）：" + ec.what());
        }
    }

    /// @brief      开始函数
    /// @details    启动连接的异步读取流程（readHead）
    /// @warning    须在socket连接建立后调用
    /// @note       幂等：重复调用只有第一次生效（由 started 标志判定）
    void Connection::start()
    {
        if (started.exchange(true))
        {
            return;
        }
        // 启动读取函数
        readHead();
    }

    /// @brief      只关闭socket
    /// @details    关闭 socket（shutdown + close）并复位发送状态，随后触发一次 derivedToClose()
    /// @warning    内部使用，禁止外部直接调用；调用前必须清空所有发送
    /// @note       由 closeSocket 标志保证只执行一次
    void Connection::socketClose()
    {
        // 检查socket状态
        if (closeSocket.exchange(true))
        {
            return;
        }

        Utils::Out::outMsg("正在关闭socket");

        // 关闭socket时查看错误码
        boost::system::error_code ec;
        socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket.close(ec);

        // 对变量置空
        sending = false;
        queueBufLen = 0;

        // 防止多次通知关闭
        if (!closeNotified.exchange(true))
        {
            derivedToClose();
        }
    }

    /// @brief      正常退出时调用此函数
    /// @details    线程安全：post 到 IO 线程执行 connectionClose()，先把发送队列中已排队的消息发完再关闭 socket
    /// @warning    调用后连接不再接收新消息，最终断开
    /// @note       幂等，可重复调用（"是否已关过"由 IO 线程内的 closing.exchange 判定）
    void Connection::stopToClose()
    {
        // 是否"已经关过"由 IO 线程内的 closing.exchange 判定，
        // 这里不再做跨线程的 test-then-act，重复调用完全安全（幂等）。
        auto self = shared_from_this();
        // io线程发送connectionClose()函数
        boost::asio::post(socket.get_executor(), [this, self]() { connectionClose(); });
    }

    /// @brief      推进关闭流程
    /// @details    幂等：有写在飞则等写完成回调续接；队列非空则先发完；都满足后关闭 socket
    /// @warning    只能在 IO 线程内执行；外部线程请调用 stopToClose()
    /// @note       重复调用只有第一次生效（closing.exchange 判定）
    void Connection::connectionClose()
    {
        // 只允许第一次调用推进关闭流程
        if (closing.exchange(true))
        {
            return;
        }

        Utils::Out::outMsg("正在关闭Session");

        if (sending)
        {
            // 有写操作在飞：等它的完成回调调用 continueClose() 续接
            return;
        }

        if (!sendQueue.empty())
        {
            // 优雅关闭：先把队列里的消息发完，doSend() 完成后会走 continueClose()
            doSend();
            return;
        }

        socketClose();
    }

    /// @brief      续接关闭流程（只由写完成回调调用：此时不再做 closing 早退判断）
    /// @details    未进入关闭流程时直接返回；仍有写在飞则等待下一次完成回调；队列发完后关闭 socket
    /// @note       仅在 IO 线程内调用
    void Connection::continueClose()
    {
        // 只由写完成回调调用；未进入关闭流程时什么都不做
        if (!closing.load())
        {
            return;
        }

        if (sending)
        {
            // 还有写操作在飞（doSend 已续上，等下一次完成回调）
            return;
        }

        if (!sendQueue.empty())
        {
            doSend();
            return;
        }

        socketClose();
    }

    /// @brief      出错时调用此函数
    /// @details    直接清空发送队列、置位 closing 并关闭 socket，不等待队列发完
    /// @warning    须在 IO 线程内调用；调用后连接不可用，队列中未发送的消息会被丢弃
    /// @note       已关闭（closeSocket 为真）时直接返回，避免重复清理
    void Connection::errToClose()
    {
        if (closeSocket.load())
        {
            return; // 已经关过，避免重复清理
        }

        // 清理所有的发送队列
        sendQueue.clear();
        // 置closing为true
        closing = true;

        // 调用socket直接关闭
        socketClose();
    }

    /// @brief      读取头部
    /// @details    异步读取消息头部（ID与长度），校验后进入消息体读取
    /// @warning    读取失败或长度非法（<=0 或超过 MAX_LENGTH）会关闭连接
    /// @note       读取前会重建 recvNode，长度固定为 HEAD_LENGTH
    void Connection::readHead()
    {
        // 保活
        auto self = shared_from_this();

        try
        {
            // 申请接收缓存
            // 使用共享指针
            recvNode = std::make_shared<RecvNode>(HEAD_LENGTH);
        }
        catch (const std::exception& e)
        {
            Utils::Out::outErr("数据清理失败，错误码！" + std::string(e.what()));

            // 申请失败，则关闭连接
            errToClose();

            return;
        }
        catch (...)
        {
            Utils::Out::outErr("未知错误");

            // 申请失败，则关闭连接
            errToClose();

            return;
        }

        // 读取数据
        boost::asio::async_read(socket, boost::asio::buffer(recvNode->getBuf(), recvNode->getTotalLen()),
                                [this, self](boost::system::error_code ec, std::size_t)
                                {
                                    // 看看是否有错误
                                    if (ec)
                                    {
                                        // 主动关闭引发的取消：正常流程，静默返回
                                        if (ec == boost::asio::error::operation_aborted)
                                        {
                                            return;
                                        }

                                        if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset)
                                        {
                                            Utils::Out::outMsg("对端已断开连接");
                                        }
                                        else
                                        {
                                            Utils::Out::outErr("读取失败：" + ec.what());
                                        }

                                        errToClose();
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
                                        Utils::Out::outErr("收到的消息的长度错误，请通知客户端修复后重试");

                                        errToClose();

                                        return;
                                    }

                                    Utils::Out::outMsg("解析完成！ID：" + std::to_string(msg_id) + "，长度：" +
                                                       std::to_string(msg_len));
                                    // 判断是否处于关闭状态
                                    if (closing)
                                    {
                                        Utils::Out::outMsg("正处在关闭连接,拒绝接收新消息");

                                        // 关闭连接
                                        stopToClose();

                                        return;
                                    }

                                    // 启用读取消息体
                                    readBody(msg_id, static_cast<int>(msg_len));
                                });
    }

    /// @brief      读取消息体
    /// @details    异步读取消息体内容，读取完成后回调 recvToWork()，随后继续读取下一个头部
    /// @param[in] msg_id 消息全局唯一ID
    /// @param[in] msg_len 消息体长度
    /// @warning    业务回调抛出的异常会被捕获并关闭连接
    /// @note       入口处会再次检查 closing；处于关闭流程时丢弃本条消息并走优雅关闭
    void Connection::readBody(unsigned long long msg_id, int msg_len)
    {
        // 检查是否在关闭状态
        if (closing)
        {
            Utils::Out::outErr("收到消息，但是正在关闭连接，拒绝接收消息");

            // 启动关闭流程
            stopToClose();
            return;
        }

        // 保活
        auto self = shared_from_this();

        // 重建接收缓存，避免上一条消息的残留影响
        try
        {
            // 申请接收缓存
            // 使用共享指针
            recvNode = std::make_shared<RecvNode>(msg_id, msg_len);
        }
        catch (const std::exception& e)
        {
            Utils::Out::outErr("数据清理失败，错误码！" + std::string(e.what()));

            errToClose();

            return;
        }
        catch (...)
        {
            Utils::Out::outErr("未知错误");

            // 申请失败，则关闭连接
            errToClose();

            return;
        }

        // 接收消息
        boost::asio::async_read(socket, boost::asio::buffer(recvNode->getBuf(), recvNode->getTotalLen()),
                                [this, self, msg_id](boost::system::error_code ec, std::size_t)
                                {
                                    // 判断是否有异常
                                    if (ec)
                                    {
                                        Utils::Out::outErr("出现错误：" + ec.what() + "关闭连接");
                                        // 关闭连接
                                        errToClose();
                                        return;
                                    }

                                    // 设置目标长度
                                    recvNode->setCurLen(recvNode->getTotalLen());

                                    // 消息转换为string类型
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
                                        errToClose();
                                    }
                                    catch (...)
                                    {
                                        Utils::Out::outErr("未知异常");
                                        errToClose();
                                    }
                                    // socket 仍打开且未进入关闭流程
                                    if (socket.is_open() && !closing)
                                    {
                                        // 继续读取下一个消息头
                                        readHead();
                                    }
                                });
    }

    /// @brief      发送任务创建
    /// @details    外部发送函数，显式指定 msg_id，校验通过后内部转调 send() 投递到 IO 线程
    /// @param[in] msg_id 消息全局唯一ID
    /// @param[in] msg 消息序列化字符串
    /// @return     已成功投递返回 true；消息为空或超过 MAX_LENGTH 返回 false
    /// @warning    线程安全，可在业务线程直接调用；消息非法时只返回 false，不会抛出异常
    /// @note
    bool Connection::toSend(unsigned long long msg_id, const std::string& msg)
    {
        // 判断消息体是否合法
        if (msg.empty())
        {
            return false;
        }
        else if (msg.size() > static_cast<std::size_t>(MAX_LENGTH))
        {
            return false;
        }

        // 加入发送队列
        send(msg_id, msg);
        return true;
    }

    /// @brief      发送消息到发送队列
    /// @details    把消息封装为发送任务（12 字节消息头 + 消息体）并加入发送队列，线程安全，可在外部线程调用
    /// @param[in] msg_id 消息全局唯一ID
    /// @param[in] msg 消息序列化字符串
    /// @warning    队列积压（queueBufLen + 本条消息体长度）超过 maxSendBufLen 时，在 IO 线程内抛出
    /// std::invalid_argument
    /// @note       任务 post 到 IO 线程执行，实际入队与发送都在 IO 线程内完成
    void Connection::send(unsigned long long msg_id, std::string msg)
    {
        // 保活获取自身this指针
        auto self = shared_from_this();

        // 获得其他线程的发送调用
        boost::asio::post(
            socket.get_executor(),
            [this, self, msg_id, msg = std::move(msg)]() mutable
            {
                // 检查是否在关闭状态
                if (closing)
                {
                    Utils::Out::outErr("准备发送消息，但是正在关闭连接，拒绝添加任务到发送队列");

                    stopToClose();

                    return;
                }

                // 构建发送任务
                auto send_node = std::make_shared<SendNode>(msg_id, HEAD_LENGTH + static_cast<int>(msg.size()));

                // 获取消息缓存空间
                char* buf = send_node->getBuf();

                // 转换字节序（长度用32位，ID用64位）
                uint32_t net_msg_len = htonl(static_cast<int>(msg.size()));
                uint64_t net_msg_id = htonll(static_cast<uint64_t>(msg_id));

                // 写入缓存（写ID，写长度，写消息）
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
                const std::size_t frameLen = static_cast<std::size_t>(HEAD_LENGTH) + msg.size();

                if (queueBufLen < 0)
                {
                    queueBufLen = 0;
                } // 兜底历史漂移

                // 限流：先判断再入队，全部用 size_t
                if (static_cast<std::size_t>(queueBufLen) + frameLen > static_cast<std::size_t>(maxSendBufLen))
                {
                    Utils::Out::outErr("发送队列积压达上限 " + std::to_string(maxSendBufLen) +
                                       " 字节，丢弃本条消息（msg_id=" + std::to_string(msg_id) + "）");
                    return;
                }

                // 放入发送队列中
                sendQueue.push_back(send_node);
                // 与出队同一口径
                queueBufLen += static_cast<int>(frameLen);

                updateTime();

                // 判断是否在发送状态，不是就启动发送，是则等待
                if (!sending)
                {
                    try
                    {
                        // 启动发送队列
                        doSend();
                    }
                    catch (const std::exception& e)
                    {
                        Utils::Out::outErr("出现错误" + std::string(e.what()));

                        errToClose();
                    }
                    catch (...)
                    {
                        Utils::Out::outErr("未知错误");

                        errToClose();
                    }
                }
            });
    }

    /// @brief      发送消息
    /// @details    从发送队列取出队首任务并异步发送，发送完成后继续取下一条
    /// @warning    仅在 IO 线程内调用；发送失败会丢弃剩余队列并关闭连接
    /// @note       队列为空时复位 sending，并在关闭流程中调用 continueClose()
    void Connection::doSend()
    {
        // 判断是否有发送的消息
        if (sendQueue.empty())
        {
            // 将发送状态变量更新
            sending = false;
            // 队列已空且正处于关闭流程：继续推进关闭
            if (closing)
            {
                stopToClose();
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
                                         errToClose();
                                         return;
                                     }

                                     // 关闭流程可能已提前清空队列；如果当前这条任务仍在队头，
                                     // 才允许弹出，避免 deque empty 的 Debug 断言。
                                     if (!sendQueue.empty() && sendQueue.front() == send_node)
                                     {
                                         queueBufLen -= send_node->getCurLen();
                                         sendQueue.pop_front();
                                         if (queueBufLen < 0)
                                         {
                                             queueBufLen = 0;
                                         }
                                     }

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
                                             continueClose();
                                         }
                                     }
                                 });
    }
} // namespace Net