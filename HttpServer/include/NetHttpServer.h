/// @file        NetHttpServer.h
/// @brief       HTTP 服务端（基于 Boost.Beast，只做报文收发，不含路由）
/// @details     在 TCP 的 Session / Server 之上叠加一层 HTTP 协议处理：
///              解析请求 → 交给业务回调 → 业务通过会话接口回包。
///              路由、鉴权、业务分发一律不做：方法、目标（含 query）、请求体原样交给业务层。
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "NetConnection.h"
#include "NetServer.h"

namespace Net
{
    namespace Server
    {
        /// @namespace  HttpServer
        /// @brief      HTTP 服务端子模块
        /// @details    在 TCP 的 Session / Server 之上叠加一层 HTTP 协议处理
        /// @note
        namespace HttpServer
        {
            /// @brief 存储一次请求的上下文
            /// @details 名字沿用历史（最早只放 url）；路由删掉之后，这里只保留协议层信息：
            ///          方法、目标、版本、请求体、复用意愿
            /// @warning 每条请求开始时会整体清空，业务层若要异步使用请自行拷贝
            /// @note
            struct Url
            {
                /// @brief 请求方法（"GET" / "POST" / "PUT" / "DELETE" ...）
                std::string method;

                /// @brief 请求目标（如 "/api/user?id=1"，含 query，未做百分号解码）
                std::string target;

                /// @brief HTTP 版本（10 / 11）
                unsigned version = 11;

                /// @brief 请求体（chunked 已由 Beast 解码成明文）
                std::string body;

                /// @brief 客户端是否希望复用连接（HTTP/1.1 默认 true）
                bool keep_alive = true;
            };

            /// @brief HTTP 会话前向声明
            class HttpSession;

            /// @brief 业务回调类型
            /// @details 与前端无关，是通用业务回调
            /// @param[in] session 当前会话（业务层用它回包）
            /// @param[in] url 请求上下文（方法/目标/版本/请求体/复用意愿）
            /// @warning 回调在 IO 线程内执行，禁止长时间阻塞；慢业务请切到工作线程，
            ///          算完后用 session->httpAsyncReplay(body) 回包（该接口线程安全）
            /// @note 返回值为 void，与 TCP 侧的 HandleFunction 保持一致：回包一律走会话接口。
            ///       在回调内直接调用 httpAsyncReplay 也安全——内部是投递到 IO 线程，
            ///       本回调返回后立刻发送；若回调返回时还没回包，会话保持忙状态等着业务层补发
            using HttpHandleFunction = std::function<void(const std::shared_ptr<HttpSession>&, const Url&)>;

            /// @brief      HTTP 会话
            /// @details    一条 TCP 连接上的 HTTP 请求循环：
            ///             读请求头 → （有 body 时）读请求体 → 业务回调 → 业务回包 →
            ///             keep-alive 则回到读请求头，否则关闭连接
            /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
            /// @warning    同一个 io_context 只允许由一个线程 run()：会话内部状态按单线程 IO 设计
            /// @note
            class HttpSession : public Net::Server::Session
            {
            public:
                /// @brief      构造函数
                /// @details    接管 socket，并把空闲超时与发送上限交给基类；随后记一次活动时间
                /// @param[in] io 存储 io_context
                /// @param[in] sock 已 accept 的连接 socket（内部 move 给基类）
                /// @param[in] HF TCP 侧回调（HTTP 链路不走基类的二进制消息，本模块不使用，
                ///            传 nullptr 即可；保留该参数只为与 Session 的构造签名保持对称）
                /// @param[in] timeout 空闲超时时间（秒，传 0 时基类回退为 60）
                /// @param[in] maxSendBufLen 发送队列积压上限（HTTP 不使用基类发送队列，仅为满足基类构造）
                /// @param[in] maxHttpSendBufLen 请求头 / 请求体的长度上限（非正数回退为 10MB）
                /// @warning    构造完成后必须马上交给 shared_ptr 持有，否则 start() 里的
                ///             shared_from_this() 会抛 std::bad_weak_ptr
                /// @note
                HttpSession(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock,
                            std::shared_ptr<HttpHandleFunction> HF, long long timeout, int maxSendBufLen,
                            int maxHttpSendBufLen);

                /// @brief      开始（或继续）读取下一条 HTTP 请求
                /// @details    重建解析器 → 清空请求上下文 → 异步读请求头；
                ///             响应写完且允许 keep-alive 时会被再次调用，构成请求循环
                /// @warning    须在连接建立后、且对象已由 shared_ptr 持有时调用
                /// @note       重写基类虚函数：不走基类「读 12 字节二进制头」的链路
                void start() override;

                /// @brief      发送 HTTP 响应报文
                /// @details    构造响应并异步写入 socket；写完按 keep-alive 决定复用连接还是关闭
                /// @param[in] body 响应体
                /// @param[in] st 状态码（默认 200）
                /// @return     报文已交给 socket 返回 true；本条请求已经回过包、本次被丢弃返回 false
                /// @warning    必须在 IO 线程内调用；业务线程请用 httpAsyncReplay
                /// @note       每条请求只回一次包，重复调用会被丢弃；
                ///             请求体没读完时不会复用连接（提前拒绝也走这里）。
                ///             返回 false 时调用方可以据此关掉卡住的连接（空闲巡检就是这么做的）
                bool httpReplay(const std::string& body,
                                boost::beast::http::status st = boost::beast::http::status::ok);

                /// @brief      线程安全地发送响应
                /// @details    任意线程均可调用，内部 post 到 IO 线程执行；
                ///             用于业务回调返回后，业务完成时主动回包
                /// @param[in] body 响应体（空串会被忽略）
                /// @warning    会话已关闭、或当前没有请求在等回包时，本次调用被丢弃
                /// @note
                void httpAsyncReplay(const std::string& body);

                /// @brief      标记「本次响应发完就关闭连接」
                /// @details    供服务器空闲巡检使用：既能把 408 之类的报文回给客户端，
                ///             又能保证回完就断开，不会把连接留在半死不活的状态
                /// @warning    只影响下一次 httpReplay 复不复用连接，不影响已经开始写出的报文
                /// @note       线程安全（原子标志）
                void markCloseAfterReply();

                /// @brief 正常的关闭连接
                /// @details 走基类的优雅关闭；会话表里的清理由服务器的巡检线程按 isClosed() 回收
                /// @warning 线程安全（内部 post 到 IO 线程）
                /// @note
                void closeSession() override;

                /// @brief  是否在忙（正在处理请求，含等待业务异步回包、响应写出中）
                /// @return 忙返回 true
                /// @note   供空闲巡检判断，避免误杀正在等业务回包的会话
                bool busing() const;

            protected:
                /// @brief 错误的关闭连接
                /// @details 走基类的出错关闭（不等发送队列，直接断开）
                /// @warning 线程安全（内部 post 到 IO 线程）
                /// @note
                void errCloseSession();

            private:
                /// @brief      处理请求
                /// @details    置忙标记后调用业务回调；回调是否已同步回包由 replied 标记判断
                /// @warning    回调在 IO 线程内执行，业务里禁止阻塞
                /// @note       仅 IO 线程内调用；回调返回后不再做任何事（回包与关闭都由回调触发）
                void handleToWork();

                /// @brief      读取请求体
                /// @details    两段式读取的第二段：在同一个 parser 上续读，读完（含 chunked 解码）后进业务
                /// @warning    判断「要不要读 body」必须用 parser->is_done()，不能用 Content-Length ——
                ///             chunked 请求根本没有这个头
                /// @note       仅 IO 线程内调用
                void readBody();

                /// @brief 出错关闭的实际执行体
                void closeWithError();

                /// @brief 获得自身指针
                /// @return 返回自己的共享指针
                /// @warning 仅在对象由 shared_ptr 持有时可用
                /// @note
                std::shared_ptr<HttpSession> getSession();

                /// @brief 请求解析器（每条请求重建，避免上一条请求的状态残留）
                std::unique_ptr<boost::beast::http::request_parser<boost::beast::http::string_body>> parser;

                /// @brief 读缓冲（跟随连接复用，保存半包与多读的字节，任何时候都不要手动清空）
                boost::beast::flat_buffer buffer;

                /// @brief 是否正在处理请求（避免巡检线程误杀正在等待业务返回的会话）
                std::atomic<bool> busy = false;

                /// @brief 本条请求是否已经回过包
                /// @details 一条请求只允许写出一条响应报文：重复写会把两条响应拼到同一条流上
                std::atomic<bool> replied = false;

                /// @brief 本次响应发完后是否关闭连接（空闲超时通知业务层用）
                std::atomic<bool> closeAfterReply = false;

                /// @brief 当前请求的上下文（业务层只读）
                Url url = Url{};

                /// @brief 业务回调
                std::shared_ptr<HttpHandleFunction> HHF = nullptr;

                /// @brief 请求头 / 请求体的长度上限
                int maxHttpSendBufLen = 0;
            };

            /// @brief      HTTP 接收服务端
            /// @details    只负责 accept 与生命周期管理，协议处理全部在 HttpSession 内完成：
            ///             accept 新连接 → 创建 HttpSession → 启动读循环；
            ///             另有一个巡检线程周期性关闭空闲超时的会话、回收已关闭的会话
            /// @warning    必须由 shared_ptr 持有（内部使用 shared_from_this() 保活异步回调）
            /// @warning    ep 只用于满足基类 Server 的构造（基类的 acceptor 本模块不使用）：
            ///             传给基类的端点端口被改成 0，由系统分配临时端口，因此 ep 的端口
            ///             与 http_port 相同也不会冲突
            /// @note
            class HttpServer : public Net::Server::Server
            {
            public:
                /// @brief      构造函数
                /// @details    完成 HTTP 监听器的 open / reuse_address / bind / listen
                /// @param[in] io 存储 io_context
                /// @param[in] ep 监听地址（只有地址部分生效，基类只把它当作地址来源）
                /// @param[in] epHttp HTTP 监听端点（地址与端口都生效）
                /// @param[in] HHF HTTP 业务回调（为空时所有请求回 500）
                /// @param[in] HF TCP 侧回调（HTTP 链路不使用，仅为兼容基类构造保留）
                /// @param[in] timeOut 基类会话的空闲超时（秒）
                /// @param[in] httpTimeOut HTTP 会话的空闲超时（秒）
                /// @param[in] checkTime 基类巡检周期（秒）
                /// @param[in] httpCheckTime HTTP 巡检周期（秒）
                /// @param[in] maxSendBufLen 基类发送队列上限（HTTP 链路不使用）
                /// @param[in] maxHttpSendBufLen 每个 HTTP 会话的请求头 / 请求体上限（字节）
                /// @warning    open / bind / listen 失败会抛 boost::system::system_error（与基类一致）；
                ///             非正数入参会回退为默认值并打印错误日志
                /// @note
                HttpServer(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep,
                           boost::asio::ip::tcp::endpoint epHttp, std::shared_ptr<HttpHandleFunction> HHF = nullptr,
                           std::shared_ptr<HandleFunction> HF = nullptr, long long timeOut = 60,
                           long long httpTimeOut = 60, long long checkTime = 60, long long httpCheckTime = 60,
                           int maxSendBufLen = 1024 * 1024 * 10, int maxHttpSendBufLen = 1024 * 1024 * 10);

                /// @brief 析构函数
                /// @details 置停止标志、关闭监听器与全部会话，再 join 巡检线程
                /// @note
                ~HttpServer() override;

                /// @brief  启动
                /// @details 等价于「开启 accept + 启动巡检线程」。这里故意隐藏基类的 start()（非虚），
                ///          避免有人误启动基类「二进制协议」的 accept 循环
                /// @warning 调用者必须已经用 shared_ptr 持有本对象
                /// @note    幂等：已在运行时只打印提示；stop() 后重启会先 join 掉上一轮的巡检线程
                void start() override;

                /// @brief      停止服务
                /// @details    置停止标志并 notify 唤醒巡检线程，再 post 到 IO 线程关闭监听器与全部会话
                /// @warning    调用后服务器不再接受新连接，须重新构造使用
                /// @note       幂等，可重复调用
                void stop() override;

            private:
                /// @brief 关闭所有的Http
                void closeHttpAll();

                /// @brief      发起一次异步 accept
                /// @details    成功则创建会话、放进会话表并启动读循环；无论成败都会续接下一次 accept
                /// @warning    须在 io_context 运行后调用；acceptor 被关闭导致的取消错误会静默忽略
                /// @note       仅 IO 线程内调用
                void startAccept();

                /// @brief      巡检线程主体
                /// @details    按 httpCheckTime 周期睡眠（可被 stop() 立即唤醒），醒来后 post 到 io_context：
                ///             给空闲超时的会话回 408 并关闭，随后移除已关闭的会话
                /// @warning    在独立线程中运行，禁止直接访问只在 IO 线程内读写的 httpSessions
                /// @note       每轮都通过 selfWeak 取保活指针，取不到说明 HttpServer 已析构，直接退出
                void clearHttpSession();

                /// @brief HTTP 监听器
                boost::asio::ip::tcp::acceptor httpAcceptor;

                /// @brief HTTP 会话集合（仅在 IO 线程内读写）
                std::vector<std::shared_ptr<HttpSession>> httpSessions;

                /// @brief 存储http的回调
                std::shared_ptr<HttpHandleFunction> HHF = nullptr;

                /// @brief 存储 HTTP 会话的空闲超时时间（秒）
                long long httpTimeOut = 0;

                /// @brief 存储巡检线程的轮询时间（秒）
                long long httpCheckTime = 0;

                /// @brief 记录每个 http session 的请求头 / 请求体上限（字节，非正数时回退为 10MB）
                int maxHttpSendBufLen = 0;

                /// @brief 记录当前的运行状态
                std::atomic<bool> httpRunning = false;

                /// @brief 用于http巡检线程
                std::thread httpClearSessionThread;

                /// @brief 巡检线程互斥锁
                /// @details 与 queueHttpCV 配合保护 httpRunning 标志，并保证巡检线程的等待可被打断
                /// @note
                std::mutex queueHttpMutex;

                /// @brief 巡检线程条件变量
                /// @details 巡检线程按 httpCheckTime 周期等待；stop() 时 notify_all 立即唤醒它退出
                /// @note
                std::condition_variable queueHttpCV;

                /// @brief 标记此服务是否已经启动过了
                std::atomic<bool> startingHttp = false;

                /// @brief httpAcceptor重连错误
                unsigned int httpAcceptorConnectionCount = 0;
            };
        } // namespace HttpServer
    } // namespace Server
} // namespace Net
