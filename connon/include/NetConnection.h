/// @file        NetConnection.h
/// @brief       网络通讯连接核心（消息缓存、读写、发送队列）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026
#pragma once
#include <memory>
#include <string>
#include <deque>
#include <atomic>
#include <cstdint>

#include <boost/asio.hpp>

namespace Net
{
    /// @brief      消息ID长度
    /// @details    消息头部中消息ID所占字节数
    /// @note
    constexpr short HEAD_ID_LENGTH = 8;

    /// @brief      消息长度字段长度
    /// @details    消息头部中「消息长度」字段所占字节数
    /// @note
    constexpr short HEAD_LEN_LENGTH = 4;

    /// @brief      消息头部长度
    /// @details    消息ID长度与消息长度字段长度之和（8 + 4 = 12 字节）
    /// @note       协议头部只有这两个字段，不含请求/目标服务器ID等其它内容
    constexpr short HEAD_LENGTH = HEAD_ID_LENGTH + HEAD_LEN_LENGTH;

    /// @brief      最大消息长度
    /// @details    单条消息允许的最大长度（1M）
    /// @warning    超出该长度视为非法消息
    /// @note
    constexpr int MAX_LENGTH = 1024 * 1024;

    /// @brief      保留消息 ID：连接测试
    /// @details    客户端连接成功后发的那条 "check connection" 用它；
    ///             服务端超时提醒用 Net::Server::kServerTimeoutMsgId(1)/kServerBusyTimeoutMsgId(2)，
    ///             业务消息建议从 1000 起
    /// @note       协议本身不校验 ID 区间，这里只是约定
    inline constexpr unsigned long long kConnCheckMsgId = 0ULL;

    /// @brief      消息体缓存
    /// @details    收发数据共用的逻辑层缓冲区，统一管理缓冲区指针、总长度、读取位置与消息ID，
    ///             派生出 RecvNode（接收）与 SendNode（发送）
    /// @note
    class MsgNode
    {
    public:
        /// @brief      构造函数（只有长度）
        /// @details    委托到 MsgNode(msg_id, max_len)，消息ID传 -1ULL 表示无效值
        /// @param[in] max_len 缓存的最大长度
        /// @warning    禁止传非正数，否则抛出 std::invalid_argument
        /// @note
        MsgNode(int max_len);

        /// @brief      构造函数（有消息ID和长度）
        /// @details    指定消息ID与缓存长度，并按长度申请缓冲区（内部调用 clear(max_len)）
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] max_len 缓存的最大长度
        /// @warning    禁止传非正数，否则抛出 std::invalid_argument
        /// @note
        MsgNode(unsigned long long msg_id, int max_len);

        /// @brief      析构函数
        /// @details    释放消息体缓存
        /// @note
        virtual ~MsgNode();

        /// @brief      清空缓存
        /// @details    将缓存内容整块置零并复位读取位置
        /// @warning    缓冲区为空指针或长度非法时抛出 std::invalid_argument
        /// @note
        void clear();

        /// @brief      重新分配缓存区大小
        /// @details    释放旧缓冲区，按 len 重新申请（多申请 1 字节并把末位置 '\0'，便于按 C 字符串使用），
        ///             随后调用 clear() 清零并复位读取位置
        /// @param[in] len 新的缓存区长度（字节）
        /// @warning    禁止传非正数，否则抛出 std::invalid_argument
        /// @note       构造函数内部会调用；也可用于复用同一个缓存对象
        void clear(int len);

        /// @brief      设置当前读取位置
        /// @param[in] len 当前读取到的位置
        /// @note
        void setCurLen(int len);

        /// @brief      设置消息ID
        /// @param[in] msg_id 消息全局唯一ID
        /// @note
        void setID(unsigned long long msg_id);

        /// @brief      获取缓存区指针
        /// @return     指向缓冲区首地址的指针
        /// @warning    禁止释放或越界访问
        /// @note
        char* getBuf();

        /// @brief      获取当前读取位置
        /// @return     当前读取位置
        /// @note
        int getCurLen() const;

        /// @brief      获取缓存区总长度
        /// @return     缓冲区总长度
        /// @note
        int getTotalLen() const;

        /// @brief      获取消息ID
        /// @return     消息全局唯一ID
        /// @note
        unsigned long long getID() const;

        /// @brief      禁用拷贝构造函数
        /// @details    消息体缓存独占资源，禁止拷贝
        /// @note
        MsgNode(const MsgNode&) = delete;

        /// @brief      禁用拷贝赋值函数
        /// @details    消息体缓存独占资源，禁止拷贝赋值
        /// @note
        MsgNode& operator=(const MsgNode&) = delete;

    protected:
        /// @brief      消息体缓存
        /// @details    存储消息数据的缓冲区
        /// @warning    由本类管理，禁止外部释放
        /// @note
        char* buf = nullptr;

        /// @brief      缓存数据大小
        /// @details    缓冲区的总长度
        /// @note
        int total_len = 0;

        /// @brief      当前读取位置
        /// @details    当前已读取/写入到的位置
        /// @note
        int cur_len = 0;

        /// @brief      消息ID
        /// @details    消息全局唯一ID
        /// @note
        unsigned long long msg_id = 0;
    };

    /// @brief      接收节点
    /// @details    继承自 MsgNode，用于读取数据时构造的逻辑层缓存
    /// @note
    class RecvNode : public MsgNode
    {
    public:
        /// @brief      构造函数（有消息ID和长度）
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] max_len 缓存的最大长度
        /// @note
        RecvNode(unsigned long long msg_id, int max_len);

        /// @brief      构造函数（只有长度）
        /// @param[in] max_len 缓存的最大长度
        /// @note
        RecvNode(int max_len);

    private:
    };

    /// @brief      发送节点
    /// @details    继承自 MsgNode，用于发送数据时构造的逻辑层缓存
    /// @note
    class SendNode : public MsgNode
    {
    public:
        /// @brief      构造函数
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] max_len 缓存的最大长度
        /// @note
        SendNode(unsigned long long msg_id, int max_len);

    private:
    };

    /// @brief      连接基类(抽象类)
    /// @details    基于 Boost.Asio 封装异步读取（readHead / readBody）与异步发送（send / doSend），
    ///             派生类只需实现业务回调与关闭清理
    /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
    /// @warning    必须重写 recvToWork() 与 derivedToClose() 两个纯虚函数
    /// @warning    同一个 io_context 只允许由一个线程 run()：sending / sendQueue 等内部状态都按单线程 IO 设计
    /// @note
    class Connection : public std::enable_shared_from_this<Connection>
    {
    public:
        /// @brief      构造函数
        /// @details    唯一的构造函数，接管 socket 并初始化内部状态
        /// @param[in] socket 连接的socket（按值传入，内部 move）
        /// @param[in] io 保存会话的io_context
        /// @param[in] maxSendBufLen 发送队列允许积压的最大字节数
        /// @warning    io 的生命周期必须长于本连接
        /// @note
        explicit Connection(boost::asio::ip::tcp::socket socket, boost::asio::io_context& io, int maxSendBufLen);

        /// @brief      发送任务创建
        /// @details    显式指定 msg_id，用于日志追踪；校验通过后转调 send() 投递到 IO 线程
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息序列化字符串
        /// @return     已成功投递返回 true；消息为空或超过 MAX_LENGTH 返回 false
        /// @warning    线程安全，可在业务线程直接调用；消息非法时不会抛出异常
        /// @note
        bool toSend(unsigned long long msg_id, const std::string& msg);

        /// @brief      开始函数
        /// @details    启动连接的异步读取流程（readHead）
        /// @warning    须在socket连接建立后调用
        /// @note       幂等：重复调用只有第一次生效（由 started 标志判定）
        virtual void start();

        /// @brief      开启 TCP_NODELAY
        /// @details    把 socket 的 no_delay 选项置为 true，即禁用 Nagle 算法、小包立即发送；
        ///             设置失败只打印错误日志，不影响正常收发
        /// @warning    须在 start() 之前调用
        /// @note       函数名沿用历史命名（off + NoDelay），实际效果是「开启 NoDelay」
        void offNoDelay();

    protected:
        /// @brief      析构函数
        /// @details    采用默认析构
        /// @note
        virtual ~Connection() = default;

        /// @brief      更新时间
        /// @details    基类为空实现；读写完成后由基类自动调用，派生类可重写以记录最后活动时间
        /// @note       在 IO 线程内被调用
        virtual void updateTime() {};

        /// @brief      后期转到具体业务
        /// @param[in] msg_id 消息id
        /// @param[in] msg 消息体
        /// @warning    纯虚函数，必须重写；仅在 IO 线程内回调，禁止长时间阻塞
        /// @note
        virtual void recvToWork(const unsigned long long msg_id, std::string msg) = 0;

        /// @brief      关闭时的派生类清理
        /// @details    socket 关闭后由 socketClose() 回调，供派生类释放自身动态资源
        /// @warning    纯虚函数，必须重写；保证只被调用一次（由 closeNotified 保护）
        /// @note       在 IO 线程内执行
        virtual void derivedToClose() = 0;

        /// @brief      正常退出时调用此函数
        /// @details    线程安全（内部 post 到 IO 线程）：先把发送队列中已排队的消息发完，再关闭 socket（优雅关闭）
        /// @warning    调用后连接不再接收新消息，最终断开
        /// @note       幂等，可重复调用；与 errToClose() 的区别是不丢弃待发消息
        void stopToClose();

        /// @brief      出错时调用此函数
        /// @details    直接清空发送队列并关闭 socket，不等待队列发完
        /// @warning    须在 IO 线程内调用；调用后连接不可用，队列中未发送的消息会被丢弃
        /// @note       与 stopToClose() 相对：出错路径优先快速释放
        void errToClose();

        /// @brief      存储socket
        /// @details    连接使用的TCP socket（构造时由入参 move 进来）
        /// @note       仅在 IO 线程内使用；关闭由 socketClose() 统一处理
        boost::asio::ip::tcp::socket socket;

        /// @brief      发送状态
        /// @details    判断当前是否正在发送（async_write 是否在飞）
        /// @note       仅在 IO 线程内读写
        bool sending;

        /// @brief      关闭状态
        /// @details    判断当前是否处于关闭流程；置位后不再接收新消息
        /// @note       存在跨线程读写，故使用 std::atomic
        std::atomic<bool> closing;

        /// @brief      关闭通知标记
        /// @details    防止 socketClose 重复触发 derivedToClose() 回调
        /// @note       用 exchange 实现一次性置位
        std::atomic<bool> closeNotified = false;

        /// @brief      所属的 io_context
        /// @details    保存该会话使用的 io_context 引用
        /// @warning    生命周期须长于本会话
        /// @note       当前仅作引用保存；异步读写与跨线程 post 都走 socket 的 executor
        boost::asio::io_context& ioc;

    private:
        /// @brief      推进关闭流程（IO 线程内执行，幂等；重复调用只有第一次生效）
        /// @details    发送队列为空则直接关 socket；否则先把队列里的消息发完，由写完成回调走 continueClose() 续接
        /// @warning    只能在 IO 线程内执行；外部线程请调用 stopToClose()
        /// @note
        void connectionClose();

        /// @brief      续接关闭流程（只由写完成回调调用：此时不再做 closing 早退判断）
        /// @details    未进入关闭流程时直接返回；仍有写在飞则等下一次完成回调；队列发完后关闭 socket
        /// @note       仅在 IO 线程内调用
        void continueClose();

        /// @brief      只关闭socket
        /// @details    关闭 socket 并复位发送状态，随后触发一次 derivedToClose()
        /// @warning    内部使用，禁止外部直接调用
        /// @note       使用前一定要清空所有的发送；由 closeSocket 标志保证只执行一次
        void socketClose();

        /// @brief      发送消息到发送队列
        /// @details    把消息封装为发送任务并加入发送队列，线程安全，可在外部线程调用
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息序列化字符串
        /// @warning    队列积压超过 maxSendBufLen 时会在 IO 线程内抛出 std::invalid_argument
        /// @note       任务 post 到 IO 线程执行，实际入队与发送都在 IO 线程内完成
        void send(unsigned long long msg_id, std::string msg);

        /// @brief      发送消息
        /// @details    从发送队列取出队首任务并异步发送，发送完成后继续取下一条
        /// @warning    仅在 IO 线程内调用；发送失败会丢弃剩余队列并关闭连接
        /// @note
        void doSend();

        /// @brief      读取头部
        /// @details    异步读取消息头部（ID与长度），校验后进入消息体读取
        /// @warning    读取失败或长度非法（<=0 或超过 MAX_LENGTH）会关闭连接
        /// @note       读取前会重建 recvNode，长度固定为 HEAD_LENGTH
        void readHead();

        /// @brief      读取消息体
        /// @details    异步读取消息体内容，读取完成后回调 recvToWork() 并继续读取下一个头部
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg_len 消息体长度
        /// @warning    业务回调抛出的异常会被捕获并关闭连接
        /// @note       入口处会再次检查 closing，处于关闭流程时直接丢弃本条消息
        void readBody(unsigned long long msg_id, int msg_len);

        /// @brief      启动标志
        /// @details    保证 start() 只生效一次（exchange 判定）
        /// @note
        std::atomic<bool> started = false;

        /// @brief      读取缓存
        /// @details    存储当前读取的接收节点
        /// @note       仅在 IO 线程内读写
        std::shared_ptr<RecvNode> recvNode;

        /// @brief      发送队列
        /// @details    待发送任务的队列，由发送完成回调逐条取出继续发送
        /// @note       仅在 IO 线程内读写
        std::deque<std::shared_ptr<SendNode>> sendQueue;

        /// @brief      socket是否已经被关闭
        /// @details    标记 socket 是否已关闭，保证 socketClose() 只执行一次
        /// @note       存在跨线程读写，故使用 std::atomic
        std::atomic<bool> closeSocket = false;

        /// @brief      发送队列积压字节数
        /// @details    入队时按消息体长度累加（不含 12 字节消息头），发送完成后按节点总长度（含消息头）扣减；
        ///             用于在入队前与 maxSendBufLen 比较，超过即抛出 std::invalid_argument
        /// @note       仅在 IO 线程内读写
        int queueBufLen = 0;

        /// @brief      发送队列允许积压的最大字节数
        /// @details    由构造函数传入；入队时若 queueBufLen + 本条消息体长度超过该值，抛出 std::invalid_argument
        /// @note
        int maxSendBufLen = 0;
    };
} // namespace Net