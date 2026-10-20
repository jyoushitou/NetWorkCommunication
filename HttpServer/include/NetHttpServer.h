/// @file        NetHttpServer.h
/// @brief       HTTP 服务端（基于 Boost.Beast，只做报文收发，不含路由）
/// @details     在 TCP 的 Session / Server 之上叠加一层 HTTP 协议处理：
///              解析请求 → 交给业务回调 → 把回调返回的字符串当作响应体发回。
///              路由、鉴权、业务分发一律不做：方法、目标（含 query）、请求体原样交给业务层。
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
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
            namespace beast = boost::beast;
            namespace http = boost::beast::http;
            namespace net = boost::asio;

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
            /// @param[in] session 当前会话（业务层可持有它做异步回包）
            /// @param[in] url 请求上下文（方法/目标/版本/请求体/复用意愿）
            /// @return 非空字符串 = 同步回包，内容直接作为响应体；
            ///         空字符串 = 异步回包，库此时不发送任何响应，
            ///         由业务层稍后调用 session->AsyncSendResponse(body) 回包
            /// @warning 回调在 IO 线程内执行，禁止长时间阻塞；慢业务请切到工作线程，
            ///          算完后用 AsyncSendResponse 回包（该接口线程安全）
            /// @note
            using HttpHandleFunction = std::function<void(const std::shared_ptr<HttpSession>&, const Url&)>;

            /// @brief      HTTP 会话
            /// @details    一条 TCP 连接上的 HTTP 请求循环：
            ///             读请求头 → （有 body 时）读请求体 → 业务回调 → 写响应 →
            ///             keep-alive 则回到读请求头，否则关闭连接
            /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
            /// @warning    同一个 io_context 只允许由一个线程 run()：会话内部状态按单线程 IO 设计
            /// @note
            class HttpSession : public Net::Server::Session
            {
            public:
                /// @brief      构造函数
                /// @details    接管 socket，保存超时与发送上限，并初始化活动时间
                /// @param[in] io 存储 io_context
                /// @param[in] sock 已 accept 的连接 socket（内部 move 给基类）
                /// @param[in] HF 业务回调（HTTP 链路不走基类的二进制消息，传空即可，仅为满足基类构造）
                /// @param[in] timeout 空闲超时时间（秒，传 0 时基类回退为 60）
                /// @param[in] maxSendBufLen 发送队列积压上限（HTTP 不使用基类发送队列，仅为满足基类构造）
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
                /// @warning    必须在 IO 线程内调用；业务线程请用 AsyncSendResponse
                /// @note       请求体没读完时不会复用连接（提前拒绝也走这里）
                void httpReplay(const std::string& body, http::status st = http::status::ok);

                /// @brief      线程安全地发送响应
                /// @details    任意线程均可调用，内部 post 到 IO 线程执行；
                ///             用于业务回调返回空串（异步模式）后，业务完成时主动回包
                /// @param[in] body 响应体（空串会被忽略）
                /// @warning    会话已关闭、或当前没有请求在等回包时，本次调用被丢弃
                /// @note
                void httpAsyncReplay(const std::string& body);

                /// @brief 正常的关闭连接
                /// @details 走基类的优雅关闭，并把本会话从 HttpServer 的会话表里移除
                /// @warning 线程安全（内部 post 到 IO 线程）
                /// @note
                void closeSession() override;

                /// @brief 错误的关闭连接
                /// @details 走基类的优雅关闭，并把本会话从 HttpServer 的会话表里移除
                /// @warning 线程安全（内部 post 到 IO 线程）
                /// @note
                void errCloseSession();

                /// @brief  是否在忙（正在处理请求，含等待业务异步回包）
                /// @return 忙返回 true
                /// @note   供空闲清理判断，避免误杀正在等业务回包的会话
                bool busing() const;

                /// @brief      处理请求
                /// @details    置忙标记后调用业务回调；返回值非空则立刻回包，
                ///             为空则保持忙标记，等业务层异步回包
                /// @note       仅 IO 线程内调用
                void handleToWork();

            private:
                /// @brief      读取请求体
                /// @details    两段式读取的第二段：在同一个 parser 上续读，读完（含 chunked 解码）后进业务
                /// @warning    判断「要不要读 body」必须用 parser->is_done()，不能用 Content-Length ——
                ///             chunked 请求根本没有这个头
                /// @note       仅 IO 线程内调用
                void readBody();

                /// @brief 获得自身指针
                /// @return 返回自己的共享指针
                std::shared_ptr<HttpSession> getSession();

                /// @brief 请求解析器（每条请求重建，避免上一条请求的状态残留）
                std::unique_ptr<http::request_parser<http::string_body>> parser;

                /// @brief 读缓冲（跟随连接复用，保存半包与多读的字节，任何时候都不要手动清空）
                beast::flat_buffer buffer;

                /// @brief 是否正在处理请求（避免清理线程误杀正在等待微服务返回的会话）
                std::atomic<bool> busy = false;

                /// @brief 当前请求的上下文（业务层只读）
                Url url = Url{};

                /// @brief 业务回调
                std::shared_ptr<HttpHandleFunction> HHF = nullptr;

                /// @brief 最大的发送体积
                int maxHttpSendBufLen = 0;
            };

            /// @brief      HTTP 接收服务端
            /// @details    只负责 accept 与生命周期管理，协议处理全部在 HttpSession 内完成：
            ///             accept 新连接 → 创建 HttpSession → 启动读循环；
            ///             另有一个定时器周期性调用 CleanupIdleSessions 回收空闲/已关闭的会话
            /// @warning    必须由 shared_ptr 持有（内部使用 shared_from_this() 保活异步回调）
            /// @warning    ep 只用于满足基类 Server 的构造（基类的 acceptor 本模块不使用）；
            ///             若 ep 的端口恰好等于 http_port，基类会改用系统分配的临时端口，避免端口冲突
            /// @note
            class HttpServer : public Net::Server::Server
            {
            public:
                /// @brief      构造函数
                /// @details    完成 HTTP 监听器的 open / reuse_address / bind / listen
                /// @param[in] io 存储 io_context
                /// @param[in] ep 监听地址（只有地址部分生效，HTTP 端口由 http_port 决定）
                /// @param[in] http_port_ HTTP 监听端口
                /// @warning    bind/listen 失败只打印错误日志，此时 StartHttpAccept() 不会开始 accept
                /// @note
                HttpServer(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep,
                           boost::asio::ip::tcp::endpoint epHttp, std::shared_ptr<HandleFunction> HF = nullptr,
                           std::shared_ptr<HttpHandleFunction> HHF, long long timeOut = 60, long long httpTimeOut = 60,
                           long long checkTime = 60, long long httpcheckTime = 60, int maxSendBufLen = 1024 * 1024 * 10,
                           int maxHttpSendBufLen = 1024 * 1024 * 10);

                /// @brief 析构函数
                /// @details 停止接收、关闭监听器与清理定时器，并通知全部会话关闭
                /// @note
                ~HttpServer() override;

                /// @brief  启动
                /// @details 等价于 StartHttpAccept()。这里故意隐藏基类的 start()（非虚），
                ///          避免有人误启动基类「二进制协议」的 accept 循环
                /// @note
                void start();

                /// @brief      停止服务
                /// @details    关闭监听器与清理定时器，并通知全部会话关闭
                /// @warning    线程安全（内部 post 到 IO 线程执行关闭动作）
                /// @note       可重复调用
                void stop();

                /// @brief      处理一次请求（业务分发入口）
                /// @details    本层不做任何路由：直接把请求上下文交给业务回调，
                ///             回调返回什么就回什么
                /// @param[in] session 当前会话
                /// @return 响应体；空串表示业务层要异步回包
                /// @warning 在 IO 线程内调用，回调禁止长时间阻塞
                /// @note
                std::string HandleVueRequest(const std::shared_ptr<HttpSession>& session);

                /// @brief      移除并关闭指定会话
                /// @details    从会话表中摘除（内部 post 到 IO 线程，避免跨线程改 vector）
                /// @param[in] session 待移除的会话
                /// @note       可重复调用
                void RemoveSession(const std::shared_ptr<HttpSession>& session);

            private:
                /// @brief      开始接受 HTTP 连接
                /// @details    启动 accept 循环与空闲清理定时器；重复调用只有第一次生效
                /// @warning    必须已用 shared_ptr 持有本对象
                /// @note
                void StartHttpAccept();

                /// @brief      发起一次异步 accept
                /// @details    成功则创建会话并启动读循环；无论成功失败都会续接下一次 accept
                /// @note       仅 IO 线程内调用
                void startAccept();

                /// @brief      监控线程主体
                /// @details    按 checkTime 周期睡眠（可被 stop() 立即唤醒），醒来后 post 到 io_context：
                ///             关闭超时会话，并从 sessions 中移除已关闭的会话
                /// @warning    在独立线程中运行，禁止直接访问只在 IO 线程内读写的 sessions
                /// @note       每轮都通过 selfWeak 取保活指针，取不到说明 Server 已析构，直接退出
                void clearHttpSession();

                /// @brief HTTP 监听器
                boost::asio::ip::tcp::acceptor httpAcceptor;

                /// @brief HTTP 会话集合（仅在 IO 线程内读写）
                std::vector<std::shared_ptr<HttpSession>> httpSessions;

                /// @brief 存储http的回调
                std::shared_ptr<HttpHandleFunction> HHF = nullptr;

                /// @brief 存储超时时间
                int httpTimeOut = 0;

                /// @brief 存储轮询时间
                int httpCheckTime = 0;

                /// @brief 记录每个session的最大发送内存
                /// @details 下发给每个新建 Session 的发送队列积压上限，单位字节（非正数时回退为 10MB）
                int maxHttpSendBufLen = 0;

                /// @brief 记录当前的运行状态
                std::atomic<bool> httpRunning = false;

                /// @brief 用于http监控线程
                std::thread httpClearSessionThread;
            };
        } // namespace HttpServer
    } // namespace Server
} // namespace Net
