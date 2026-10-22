/// @file        NetServer.h
/// @brief       TCP 服务器与连接会话（Session / Server）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// 防止重复包含
#pragma once
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <tuple> // 本文件未直接使用（历史遗留：WaitForMessage / msgQueue 已移除，该包含可删除）
#include <ctime> // time_t（Session::lastTime）

#include <boost/asio.hpp>

#include "NetConnection.h" // Session 继承自 Connection（Server 只持有 Session，本身不继承 Connection）

/// @namespace  Net
/// @brief      网络通讯模块
/// @details    目前实现了TCP连接
/// @note
namespace Net
{
    /// @namespace  Server
    /// @brief      网络通讯服务器子模块
    /// @details    只有TCP连接
    /// @note
    namespace Server
    {
        /// @brief      会话退出关键字
        /// @details    Session 收到内容等于该字符串的消息时，直接关闭本次会话（不再进入业务回调）
        /// @note       固定为 "EXIT"，编译期常量
        inline constexpr const char* kCloseCommand = "EXIT";

        /// @brief 连接会话前向声明
        class Session;

        /// @brief 消息回调类型
        /// @details 收到消息后由该回调处理；回调无返回值，回复需在回调内主动调用 session->reply(msg_id, msg)
        /// @warning 禁止持有session裸指针，必须使用shared_ptr延长生命周期；回调内部禁止长时间阻塞
        /// @note 回调在 IO 线程内执行
        using HandleFunction = std::function<void(const std::shared_ptr<Session>& session,
                                                  const unsigned long long& msg_id, const std::string& msg)>;

        /// @brief      连接会话
        /// @details    tcp通讯的会话，负责与单个客户端收发数据
        /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
        /// @warning    同一个 io_context 只允许由一个线程 run()，会话的所有状态都按单线程 IO 设计
        class Session : public Connection
        {
        public:
            /// @brief      构造函数
            /// @details    session的构造：接管 socket，保存业务回调、超时时间与发送上限，并初始化活动时间
            /// @param[in] io 连接的io_context
            /// @param[in] sock 连接的socket（按值传入，内部 move 给基类）
            /// @param[in] HF 消息回调函数（可为空，为空时收到消息即关闭会话）
            /// @param[in] timeout 空闲超时时间（秒）
            /// @param[in] maxSendBufLen 发送队列允许积压的最大字节数
            /// @note       timeout 传 0 时回退为默认 60 秒
            Session(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock, std::shared_ptr<HandleFunction> HF,
                    long long timeout, int maxSendBufLen);

            ~Session() override = default;

            /// @brief      回复消息
            /// @details    线程安全的发送入口（内部 post 到 IO 线程），向该客户端回复一条消息
            /// @param[in] msg_id 消息全局唯一ID
            /// @param[in] msg 消息序列化字符串
            /// @warning    禁止在回调或事件循环中长时间阻塞；msg 为空时只打印错误日志并丢弃
            /// @note
            void reply(unsigned long long msg_id, const std::string& msg);

            /// @brief      关闭函数
            /// @details    置位停止标志并走基类的优雅关闭（先把发送队列发完再断开）
            /// @warning    线程安全，最终关闭在 IO 线程内完成
            /// @note
            virtual void closeSession();

            /// @brief      是否已关闭
            /// @details    判断连接是否已进入关闭流程（stop / closing）或 socket 已断开
            /// @return     已关闭返回 true，否则返回 false
            /// @warning    仅在 IO 线程内调用，避免跨线程读取非原子状态
            /// @note
            bool isClosed();

            /// @brief      判断是否超时
            /// @details    以最后一次活动时间 lastTime 为基准，与 timeout 比较
            /// @return     true超时，false未超时
            /// @note       由服务器监控线程在 IO 线程内调用
            bool timeOut();

        protected:
            /// @brief      业务处理
            /// @details    收到消息后的具体业务实现，由基类回调触发：内容为 kCloseCommand 时关闭会话，
            ///             否则转交 HF 处理（HF 为空时也关闭会话）
            /// @param[in] msg_id 消息全局唯一ID
            /// @param[in] msg 消息序列化字符串
            /// @warning    禁止在回调内部长时间阻塞，会阻塞asio事件循环
            /// @note
            void recvToWork(const unsigned long long msg_id, std::string msg);

            /// @brief      更新连接时间
            /// @details    刷新 lastTime，供超时巡检使用，避免空闲连接长期占线
            /// @note       重写基类虚函数；在每次读写操作后由基类调用，也可手动调用
            void updateTime();

            /// @brief 重写关闭最后的流程
            /// @details 用于在session最后关闭时销毁其他动态数据
            /// @warning 当前是空实现：Session 没有需要额外释放的动态资源
            /// @note 由基类 socketClose() 保证只回调一次
            void derivedToClose() override;

        private:
            // 将tosend函数降级为保护
            // 发送消息使用reply
            using Connection::toSend;

            /// @brief      获取自身的指针
            /// @details    把 shared_from_this() 向下转型为 Session 的 shared_ptr，供回调向外传递
            /// @return     Session 的 shared_ptr（static_pointer_cast 不做运行时校验，正常不会为空）
            /// @warning    仅在对象由 shared_ptr 持有时可用，否则 shared_from_this() 抛出 std::bad_weak_ptr
            std::shared_ptr<Session> getSession();

            /// @brief 回调函数
            /// @details 用于收到消息的具体消息处理（在 IO 线程内调用）
            std::shared_ptr<HandleFunction> HF = nullptr;

            /// @brief 记录超时时间
            /// @details 会话空闲超时时间，单位秒（构造函数传入，0 时回退为 60）
            long long timeout = 60;

            /// @brief 最后更新时间
            /// @details 记录最后一次读写活动的时间戳（秒），timeOut() 依据它判断空闲超时
            /// @note
            std::atomic<time_t> lastTime;

            /// @brief 停止标志
            /// @details 标识该会话是否已被请求关闭（由 closeSession 置位）
            /// @note
            std::atomic<bool> stop = false;
        };

        /// @brief      服务器端
        /// @details    负责 listen / accept 的服务器类：为每个新连接创建 Session，并启动监控线程
        ///             周期巡检（关闭超时会话、回收已关闭会话）；消息不经队列，全部由 HandleFunction 回调交给业务侧
        /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
        /// @warning    start() 依赖 shared_from_this()，必须由 shared_ptr 持有后再调用
        class Server : public std::enable_shared_from_this<Server>
        {
        public:
            /// @brief      构造函数
            /// @details    服务器的构造：完成 acceptor 的 open / set_option / bind / listen，并保存各项参数
            /// @param[in] io 服务器的io_context
            /// @param[in] ep 监听的本地端点（地址与端口）
            /// @param[in] HF 业务处理回调（回调内调用 session->reply 回复，库不会自动回包）
            /// @param[in] timeOut 会话空闲超时时间（秒），默认 60 秒
            /// @param[in] checkTime 监控线程检查会话的周期（秒），默认 60 秒
            /// @param[in] maxSendBufLen 每个会话的发送队列允许积压的最大字节数，默认 10MB
            /// @warning    须保证 io 的生命周期长于本服务器；非正数入参会回退为默认值并打印错误日志
            /// @note
            Server(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep,
                   std::shared_ptr<HandleFunction> HF = nullptr, long long timeOut = 60, long long checkTime = 60,
                   int maxSendBufLen = 1024 * 1024 * 10);

            /// @brief      析构函数
            /// @details    先 stop() 停服并关闭所有会话，再 join 监控线程
            /// @note       派生类析构会自动调用基类析构；join 后可避免 std::thread 析构时 std::terminate
            virtual ~Server();

            /// @brief 启动
            /// @details 置 running、绑定 selfWeak，随后启动监控线程（clearSession）并开始 accept
            /// @warning 会启动一个监控线程；调用者必须已经用 shared_ptr 持有本对象
            /// @note 幂等：已在运行时只打印提示；Stop() 后重启会先回收上一轮的监控线程
            virtual void start();

            /// @brief      停止服务器
            /// @details    置 running=false 并 notify_all 唤醒监控线程，再 post 到 IO 线程关闭 acceptor、逐个关闭会话
            /// @warning    调用后服务器不再接受新连接，须重新构造使用
            /// @note       幂等，可重复调用
            virtual void stop();

        protected:
            /// @brief      所属的 io_context
            /// @details    保存服务器使用的 io_context 引用
            /// @warning    生命周期须长于本服务器
            boost::asio::io_context& ioc;

            /// @brief      自身弱引用
            /// @details    供监控线程与 stop() 安全地判断 Server 是否仍存活，并保活投递到 IO 线程的任务
            /// @note       在 start() 中由 shared_from_this() 赋值
            std::weak_ptr<Server> selfWeak;

            /// @brief      运行状态标志
            /// @details    标识服务器当前是否正在运行；Stop() 置 false 后 accept / 监控线程都会退出
            /// @note       跨线程读写，故使用 std::atomic
            std::atomic<bool> running = false;

            /// @brief      会话列表
            /// @details    管理所有已建立的连接会话
            /// @warning    Session 继承自 enable_shared_from_this，必须用 shared_ptr 管理
            /// @note       仅在 IO 线程内读写
            std::vector<std::shared_ptr<Session>> sessions;

        private:
            /// @brief 打开Acceptor
            /// @details 为了支持重启的重连
            /// @param[out] ec 错误码
            /// @return 返回连接的状态
            bool openAcceptor(boost::system::error_code ec);

            /// @brief      开始接受连接
            /// @details    在 io_context 线程中被调用，异步等待并接受客户端连接，
            ///             为每个新连接创建 Session（关闭 NoDelay）后继续接受下一个连接
            /// @warning    须在 io_context 运行后调用；acceptor 被关闭导致的取消错误会静默忽略
            /// @note       accept 失败时等 100ms 重试，服务器已停止则不再重试
            void startAccept();

            /// @brief      监控线程主体
            /// @details    按 checkTime 周期睡眠（可被 stop() 立即唤醒），醒来后 post 到 io_context：
            ///             关闭超时会话，并从 sessions 中移除已关闭的会话
            /// @warning    在独立线程中运行，禁止直接访问只在 IO 线程内读写的 sessions
            /// @note       每轮都通过 selfWeak 取保活指针，取不到说明 Server 已析构，直接退出
            void clearSession();

            /// @brief      监听器
            /// @details    保存用于 listen / accept 的 acceptor
            /// @note       仅在 IO 线程内使用
            boost::asio::ip::tcp::acceptor acceptor;

            /// @brief      监控线程互斥锁
            /// @details    与 queueCV 配合保护 running 标志，并保证监控线程的等待可被打断
            /// @warning    命名沿用历史（原名 queueMutex），已与"消息队列"无关
            /// @note
            std::mutex queueMutex;

            /// @brief      监控线程条件变量
            /// @details    监控线程按 checkTime 周期等待；Stop() 时 notify_all 立即唤醒它退出
            /// @warning    命名沿用历史（原名 queueCV），与"消息到达"无关
            /// @note
            std::condition_variable queueCV;

            /// @brief      失效会话清理线程
            /// @details    定期清理失效会话的后台线程（see clearSession）
            /// @warning    赋值前必须 join 掉上一轮的线程，否则会 std::terminate
            /// @note
            std::thread clearSessionThread;

            /// @brief 回调的函数
            /// @details 整个服务器共用的业务处理回调，创建 Session 时下发给每个会话
            std::shared_ptr<HandleFunction> HF;

            /// @brief 监控线程巡检周期
            /// @details 监控线程每 checkTime 秒巡检一次会话，单位秒（构造函数传入，0 时回退为 60）
            long long checkTime = 60;

            /// @brief 会话空闲超时时间
            /// @details 下发给每个新建 Session 的空闲超时时间，单位秒（构造函数传入，0 时回退为 60）
            long long timeOut = 60;

            /// @brief 记录每个session的最大发送内存
            /// @details 下发给每个新建 Session 的发送队列积压上限，单位字节（非正数时回退为 10MB）
            int maxSendBufLen = 0;

            /// @brief 标记此服务是否已经启动过了
            std::atomic<bool> starting = false;

            /// @brief 监听节点
            boost::asio::ip::tcp::endpoint ep;
        };
    } // namespace Server
} // namespace Net