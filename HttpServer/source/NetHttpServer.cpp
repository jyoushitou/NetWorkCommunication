/// @file        NetHttpServer.cpp
/// @brief       HTTP 服务端实现（基于 Boost.Beast，只做报文收发，不含路由）
/// @details     解析请求 → 交给业务回调 → 把回调返回的字符串当作响应体发回。
///              方法、目标（含 query）、请求体全部原样交给业务层，本层不做任何分发。
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// 头文件
#include "NetHttpServer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "Utils.h"

namespace Net
{
    namespace Server
    {
        namespace HttpServer
        {
            namespace
            {
                /// @brief 单条请求体的上限（字节）；超过回 413 并断开连接
                constexpr std::uint64_t kMaxBodyBytes = 4ull * 1024 * 1024;

                /// @brief 请求头上限（字节）；Beast 默认只有 8KB，带长 Cookie / JWT 时容易不够
                constexpr std::uint32_t kMaxHeaderBytes = 16u * 1024;

                /// @brief 空闲清理的巡检周期（秒）
                constexpr long long kCleanupIntervalSeconds = 10;

                /// @brief 新建会话的空闲超时（秒），给基类 timeOut() 用
                /// @note  空闲清理走的是 last_active_ms + idle_timeout_ms，两套互不干扰
                constexpr long long kSessionTimeoutSeconds = 60;

                /// @brief 新建会话的发送队列上限（HTTP 不走基类二进制发送队列，给个常规值即可）
                constexpr int kMaxSendBufLen = 1024 * 1024 * 10;

                /// @brief 获取当前时间（steady_clock 毫秒）
                /// @return 自开机以来的毫秒数
                /// @note   只用于比较差值，绝对值没有意义
                long long NowMs()
                {
                    return std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                }

                /// @brief 构造一个最小的 JSON 错误体
                /// @param[in] msg 错误描述
                /// @return 形如 {"code":1,"msg":"..."} 的字符串
                /// @warning 业务回调返回空串表示「异步回包」，所以错误响应不能返回空串
                /// @note
                std::string ErrorBody(const char* msg)
                {
                    return std::string("{\"code\":1,\"msg\":\"") + msg + "\"}";
                }

            } // namespace

            //========== HttpSession ==========

            /// @brief      构造函数
            /// @details    接管 socket，并把空闲超时与发送上限交给基类；随后记一次活动时间
            /// @note       HTTP 链路不走基类的二进制消息回调，所以 HF 传空
            HttpSession::HttpSession(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock,
                                     std::shared_ptr<HttpHandleFunction> HF, long long timeout, int maxSendBufLen)
                : Net::Server::Session(io, std::move(sock), nullptr, timeout, maxSendBufLen)
            {
                // 创建时立即记录活动时间，防止刚建好就被清理定时器误删
                updateTime();
            }

            /// @brief      开始（或继续）读取下一条 HTTP 请求
            /// @details    重建解析器 → 清空请求上下文 → 异步读请求头；
            ///             读完头后按 parser 的状态决定是直接进业务还是继续读 body
            /// @note       重写基类虚函数，不走基类「读 12 字节二进制头」的链路
            void HttpSession::start()
            {
                // HTTP/1.1 默认 Keep-Alive：每条请求处理完后可能要复用本连接继续读下一条，
                // 这里把解析器重置为初始状态（重新 make_unique，任何 Boost 版本都兼容）
                parser = std::make_unique<http::request_parser<http::string_body>>();
                parser->body_limit(kMaxBodyBytes);     // 请求体上限，超了会得到 body_limit 错误
                parser->header_limit(kMaxHeaderBytes); // 请求头上限，超了会得到 header_limit 错误

                // 清空上一条请求的上下文
                // buffer 不能清：里面可能已经缓存了下一条请求的字节（HTTP pipelining）
                url = Url{};

                // 新请求开始，先解除忙碌标记
                busy.store(false, std::memory_order_relaxed);

                // 更新时间
                updateTime();

                // 保活：异步回调执行期间会话不能被析构
                auto self = getSession();

                // 两段式读取的第一段：只读到请求头结束，此时 body 还没有读
                boost::beast::http::async_read_header(socket, buffer, *parser,
                                                      [self](boost::system::error_code ec, std::size_t)
                                                      {
                                                          // 对端在 keep-alive 空闲期正常关闭，不是错误
                                                          if (ec == http::error::end_of_stream)
                                                          {
                                                              self->closeSession();
                                                              return;
                                                          }

                                                          // 连接错误 / 报文非法 / 头过大：流已经不可用，只能关连接
                                                          if (ec)
                                                          {
                                                              Utils::Out::outErr("HTTP 读取请求头失败：" +
                                                                                 ec.message());
                                                              self->errCloseSession();
                                                              return;
                                                          }

                                                          // 把请求头里的信息落到上下文里（业务层只认 Url）
                                                          auto& req = self->parser->get();
                                                          self->url.method = std::string(req.method_string());
                                                          self->url.target = std::string(req.target());
                                                          self->url.version = req.version();
                                                          self->url.keep_alive = req.keep_alive();
                                                          self->updateTime();

                                                          // 没有 body（GET、Content-Length: 0）→ 直接进业务
                                                          if (self->parser->is_done())
                                                          {
                                                              self->handleToWork();
                                                              return;
                                                          }

                                                          // 有 body（Content-Length > 0 或 Transfer-Encoding:
                                                          // chunked）→ 继续读
                                                          self->readBody();
                                                      });
            }

            /// @brief      读取消息体
            /// @details    两段式读取的第二段：在同一个 parser 上续读，读完（含 chunked 解码）后进业务
            /// @warning    判断「要不要读 body」必须用 parser->is_done()，不能用 Content-Length ——
            ///             chunked 请求根本没有这个头
            void HttpSession::readBody()
            {
                updateTime();

                // 保活
                auto self = getSession();

                // 读取http body
                boost::beast::http::async_read(socket, buffer, *parser,
                                               [self](boost::system::error_code ec, std::size_t)
                                               {
                                                   // 请求体超过 parser->body_limit()：剩余 body 还在流里，只能回 413
                                                   // 后断开
                                                   if (ec == http::error::body_limit)
                                                   {
                                                       Utils::Out::outErr("HTTP 请求体超过上限，返回 413");
                                                       self->httpReplay(R"({"code":413,"msg":"payload too large"})",
                                                                        http::status::payload_too_large);
                                                       return;
                                                   }

                                                   // 对端在发 body 的过程中正常关闭
                                                   if (ec == http::error::end_of_stream)
                                                   {
                                                       self->closeSession();
                                                       return;
                                                   }

                                                   if (ec)
                                                   {
                                                       Utils::Out::outErr("HTTP 读取请求体失败：" + ec.message());
                                                       self->errCloseSession();
                                                       return;
                                                   }

                                                   // chunked 已由 Beast 解码，这里拿到的是完整明文
                                                   self->url.body = self->parser->get().body();
                                                   self->updateTime();
                                                   self->handleToWork();
                                               });
            }

            /// @brief      处理请求
            /// @details    置忙标记后调用业务回调；返回值非空则立刻回包，
            ///             为空则保持忙标记，等业务层异步回包
            /// @warning    回调在 IO 线程内执行，业务里禁止阻塞
            void HttpSession::handleToWork()
            {
                // 标记忙碌：请求处理期间（包括等待业务异步回包）不允许被清理定时器关掉
                busy.store(true, std::memory_order_relaxed);
                updateTime();

                auto self = std::static_pointer_cast<HttpSession>(shared_from_this());

                std::string response;
                try
                {
                    // 调用回调
                    (*HHF)(getSession(), url);
                }
                catch (const std::exception& e)
                {
                    Utils::Out::outErr("HTTP 业务回调抛出异常：" + std::string(e.what()));
                    response = ErrorBody("internal error");
                }
                catch (...)
                {
                    Utils::Out::outErr("HTTP 业务回调抛出未知异常");
                    response = ErrorBody("internal error");
                }

                // 非空 = 同步回包
                if (!response.empty())
                {
                    httpReplay(response);
                    return;
                }

                // 空串 = 异步回包：这里什么都不发，busy 保持 true，
                // 等业务层拿到结果后调用 AsyncSendResponse() 回包
            }

            /// @brief      发送 HTTP 响应报文
            /// @details    组报文 → 异步写 → 写完按 keep-alive 决定复用连接还是关闭
            /// @param[in] body 响应体（非空）
            /// @param[in] st 状态码
            void HttpSession::httpReplay(const std::string& body, boost::beast::http::status st)
            {
                // 只有请求体读完才允许复用连接：提前拒绝（413）时流里还有未读数据，必须关
                const bool keep = url.keep_alive && parser && parser->is_done();

                // 报文对象必须活到写完成，用 shared_ptr 交给 lambda 持有；
                // 版本回显请求的版本，HTTP/1.0 的客户端不认 1.1 的默认复用语义
                auto res = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(
                    st, url.version ? url.version : 11);

                // 构建报文
                res->set(boost::beast::http::field::server, "CommonNetHttpServer");
                res->set(boost::beast::http::field::content_type, "application/json; charset=utf-8");
                // 允许跨域：前端（Vue / React）直连本服务时需要
                res->set("Access-Control-Allow-Origin", "*");
                res->set("Access-Control-Allow-Headers", "Content-Type, Authorization");
                res->set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");

                // 消息体复制到发送里
                res->body() = body;

                // 用 API 设置复用意愿：会按版本自动写/省略 Connection 头，不要手写
                res->keep_alive(keep);

                // 根据 body 自动计算 Content-Length（必须在 body 赋值之后调用）
                res->prepare_payload();

                updateTime();
                auto self = std::static_pointer_cast<HttpSession>(shared_from_this());

                http::async_write(socket, *res,
                                  [self, res](boost::system::error_code ec, std::size_t)
                                  {
                                      // 无论成功还是失败，这条请求到这里都处理完了
                                      self->busy.store(false, std::memory_order_relaxed);
                                      self->updateTime();

                                      if (ec)
                                      {
                                          Utils::Out::outErr("HTTP 发送响应失败：" + ec.message());
                                          self->errCloseSession();
                                          return;
                                      }

                                      // 报文里说了不复用（或客户端要求关闭）→ 关连接
                                      if (!res->keep_alive())
                                      {
                                          self->closeSession();
                                          return;
                                      }

                                      // 复用连接：继续读下一条请求
                                      self->start();
                                  });
            }

            /// @brief      线程安全地回复响应
            /// @details    任意线程均可调用，内部 post 到 IO 线程执行，避免跨线程碰 socket / parser
            /// @param[in] body 响应体（空串会被忽略）
            /// @warning    会话已关闭、或当前没有请求在等回包时，本次调用被丢弃
            /// @note
            void HttpSession::httpAsyncReplay(const std::string& body)
            {
                auto self = getSession();

                boost::asio::post(ioc,
                                  [self, body]()
                                  {
                                      // 空响应体没有意义，直接丢弃（约定了空串表示「异步」，不能拿来回空包）
                                      if (body.empty())
                                          return;

                                      // 会话已关闭（超时、对端断开）→ 丢弃
                                      if (self->isClosed())
                                          return;

                                      // 当前没有请求在等回包（同步模式已经回过了）→ 丢弃，避免重复响应
                                      if (!self->busy.load(std::memory_order_relaxed))
                                          return;

                                      self->httpReplay(body);
                                  });
            }

            /// @brief  是否在忙（正在处理请求，含等待业务异步回包）
            /// @return 忙返回 true
            /// @note   供空闲清理判断，避免误杀正在等业务回包的会话
            bool HttpSession::busing() const
            {
                return busy.load(std::memory_order_relaxed);
            }

            /// @brief 关闭连接
            /// @details 走基类的优雅关闭，并把本会话从 HttpServer 的会话表里移除
            void HttpSession::closeSession()
            {
                // 基类：置停止标志 + 优雅关闭（关 socket 时回调 derivedToClose）
                Session::closeSession();
            }

            void HttpSession::errCloseSession()
            {
                Session::errToClose();
            }

            //========== HttpServer ==========

            /// @brief      构造函数
            /// @details    初始化 HTTP 监听器（open / reuse_address / bind / listen）
            /// @param[in] io 存储 io_context
            /// @param[in] ep 监听地址（只有地址部分生效，HTTP 端口由 http_port 决定）
            /// @param[in] http_port_ HTTP 监听端口
            HttpServer::HttpServer(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep,
                                   unsigned short http_port_)
                : Net::Server::Server(io,
                                      // 基类构造时会 bind 这个端点，而本模块并不使用基类的 accept。
                                      // 若它与 HTTP 端口相同就会抢占端口，这里改用临时端口（0）避开
                                      (ep.port() == http_port_) ? boost::asio::ip::tcp::endpoint{ep.address(), 0} : ep,
                                      nullptr),
                  http_acceptor(io), cleanup_timer(io), http_port(http_port_)
            {
                boost::system::error_code ec;
                const boost::asio::ip::tcp::endpoint http_ep(ep.address(), http_port);

                // 打开监听器（协议跟随 ep，IPv4 / IPv6 都支持）
                http_acceptor.open(http_ep.protocol(), ec);
                if (ec)
                {
                    Utils::Out::outErr("HTTP 监听器 open 失败：" + ec.message());
                    return;
                }

                // 端口复用：进程重启时不会因为 TIME_WAIT 报「Address already in use」
                http_acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
                if (ec)
                {
                    Utils::Out::outErr("HTTP 监听器设置 SO_REUSEADDR 失败：" + ec.message());
                    return;
                }

                http_acceptor.bind(http_ep, ec);
                if (ec)
                {
                    Utils::Out::outErr("HTTP 监听器 bind 失败：" + ec.message());
                    return;
                }

                http_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
                if (ec)
                {
                    Utils::Out::outErr("HTTP 监听器 listen 失败：" + ec.message());
                    return;
                }
            }

            /// @brief      析构函数
            /// @details    停止接收、关闭监听器与清理定时器，并通知所有会话关闭
            /// @warning    这里直接做同步清理，不调用 Stop()：Stop() 内部用 shared_from_this()
            ///             保活，而析构阶段已经不存在有效的 shared_ptr
            HttpServer::~HttpServer()
            {
                http_running.store(false);

                // 关监听器与定时器：挂起的 accept / 定时回调会以 operation_aborted 回来
                boost::system::error_code ignored;
                http_acceptor.close(ignored);
                cleanup_timer.cancel(ignored);

                for (auto& session : http_sessions)
                {
                    if (!session)
                        continue;

                    // 先断开反向引用，否则会话的 Stop() 会回调 RemoveSession(shared_from_this())
                    session->http_server = nullptr;
                    session->Stop();
                }
                http_sessions.clear();
            }

            /// @brief      注册回调函数
            /// @param[in] cb 业务回调
            void HttpServer::SetVueCallBack(VueCallBack cb)
            {
                VueCallBackFunction = std::make_shared<VueCallBack>(std::move(cb));
            }

            /// @brief 设置空闲超时
            /// @param[in] idle_timeout_ms 空闲多少毫秒后关闭会话
            void HttpServer::SetIdleTimeout(long long idle_timeout_ms)
            {
                this->idle_timeout_ms = (idle_timeout_ms > 0) ? idle_timeout_ms : 60000;
            }

            /// @brief      开始接受 HTTP 连接
            /// @details    启动 accept 循环与空闲清理定时器；重复调用只有第一次生效
            void HttpServer::StartHttpAccept()
            {
                // 只允许启动一次
                bool expected = false;
                if (!http_running.compare_exchange_strong(expected, true))
                {
                    Utils::Out::outMsg("HTTP 服务已经在接收连接");
                    return;
                }

                // 构造时 bind / listen 失败，这里就没有监听器可用
                if (!http_acceptor.is_open())
                {
                    Utils::Out::outErr("HTTP 监听器未打开，无法接收连接");
                    http_running.store(false);
                    return;
                }

                // 弱引用供 accept / 定时器回调保活；要求调用方已用 shared_ptr 持有本对象
                selfWeak = std::static_pointer_cast<HttpServer>(shared_from_this());

                Utils::Out::outMsg("HTTP 服务开始监听，端口：" + std::to_string(http_port));

                doAccept();
                StartCleanupTimer();
            }

            /// @brief      发起一次异步 accept
            /// @details    成功则创建会话、放进会话表并启动读循环；无论成败都会续接下一次 accept
            void HttpServer::doAccept()
            {
                auto self = selfWeak.lock();
                if (!self || !http_running.load())
                    return;

                http_acceptor.async_accept(
                    [this, self](boost::system::error_code ec, boost::asio::ip::tcp::socket socket)
                    {
                        if (!http_running.load())
                            return;

                        if (ec)
                        {
                            // 服务器主动关闭监听器时会有 operation_aborted，属于正常退出流程
                            if (ec != boost::asio::error::operation_aborted)
                                Utils::Out::outErr("HTTP accept 失败：" + ec.message());
                        }
                        else
                        {
                            // 关闭 Nagle：HTTP 都是小包交互，减少延迟
                            boost::system::error_code ignore;
                            socket.set_option(boost::asio::ip::tcp::no_delay(true), ignore);

                            // 创建 HTTP 会话：socket 交给会话，会话通过 http_server 回调本对象
                            auto session = std::make_shared<HttpSession>(ioc, std::move(socket), nullptr,
                                                                         kSessionTimeoutSeconds, kMaxSendBufLen);
                            session->http_server = this;

                            http_sessions.push_back(session);
                            session->start();
                        }

                        // ★ 无论本次成功还是失败都要续接，
                        //   否则一次 accept 失败之后就再也不能接受新连接了
                        if (http_running.load())
                            doAccept();
                    });
            }

            /// @brief      安排下一次空闲会话清理
            /// @details    用定时器在 IO 线程内周期触发，不需要额外的轮询线程
            void HttpServer::StartCleanupTimer()
            {
                auto self = selfWeak.lock();
                if (!self || !http_running.load())
                    return;

                cleanup_timer.expires_after(std::chrono::seconds(kCleanupIntervalSeconds));
                cleanup_timer.async_wait(
                    [this, self](boost::system::error_code ec)
                    {
                        // operation_aborted = 服务器停止时取消了定时器，正常退出
                        if (ec || !http_running.load())
                            return;

                        CleanupIdleSessions(idle_timeout_ms);
                        StartCleanupTimer();
                    });
            }

            /// @brief      清理空闲超时的 HTTP 会话
            /// @details    遍历会话表：已关闭的直接移出；空闲超时且不在等业务回包的主动关闭
            /// @param[in] idle_timeout_ms 空闲多少毫秒算超时
            /// @warning    正在等业务异步回包（busy）的会话不在这里杀，
            ///             避免把慢业务的结果打断（业务层需自己保证最终回包）
            /// @note       线程安全：内部 post 到 IO 线程执行
            void HttpServer::CleanupIdleSessions(long long idle_timeout_ms)
            {
                auto self = std::static_pointer_cast<HttpServer>(shared_from_this());

                boost::asio::post(ioc,
                                  [this, self, idle_timeout_ms]()
                                  {
                                      const long long now_ms = NowMs();

                                      for (auto it = http_sessions.begin(); it != http_sessions.end();)
                                      {
                                          auto& session = *it;
                                          if (!session)
                                          {
                                              it = http_sessions.erase(it);
                                              continue;
                                          }

                                          // 已经关闭的会话：直接移出会话表（socket 由会话自己负责关闭）
                                          if (session->isClosed())
                                          {
                                              it = http_sessions.erase(it);
                                              continue;
                                          }

                                          // 空闲超时且没在等业务回包 → 主动关闭（Stop 会把自己从表里摘掉）
                                          if (!session->busing() &&
                                              now_ms - session->last_active_ms.load(std::memory_order_relaxed) >
                                                  idle_timeout_ms)
                                          {
                                              Utils::Out::outMsg("清理空闲超时的 HTTP 会话");
                                              session->Stop();
                                          }
                                          ++it;
                                      }
                                  });
            }

            /// @brief      处理一次请求（业务分发入口）
            /// @details    本层不做任何路由：直接把请求上下文交给业务回调，回调返回什么就回什么
            /// @param[in] session 当前会话
            /// @return 响应体；空串表示业务层要异步回包
            std::string HttpServer::HandleVueRequest(const std::shared_ptr<HttpSession>& session)
            {
                // 回调没注册就回错误（属于配置问题，不能返回空串，否则会被当成「异步回包」）
                if (!VueCallBackFunction || !(*VueCallBackFunction))
                {
                    Utils::Out::outErr("未设置 HTTP 业务回调函数！");
                    return R"({"code":2,"msg":"business callback not set"})";
                }

                // 把请求原样交给业务层：方法、目标、请求体都在 session->url 里
                return (*VueCallBackFunction)(session, session->url);
            }

            /// @brief      移除并关闭指定会话
            /// @details    从会话表中摘除（内部 post 到 IO 线程，避免跨线程改 vector）
            /// @param[in] session 待移除的会话
            void HttpServer::RemoveSession(const std::shared_ptr<HttpSession>& session)
            {
                auto self = std::static_pointer_cast<HttpServer>(shared_from_this());

                boost::asio::post(ioc,
                                  [this, self, session]()
                                  {
                                      auto it = std::find(http_sessions.begin(), http_sessions.end(), session);
                                      if (it != http_sessions.end())
                                      {
                                          Utils::Out::outMsg("移除 HTTP Session");
                                          http_sessions.erase(it);
                                      }
                                  });
            }

            /// @brief      停止服务
            /// @details    置停止标志、关闭监听器与定时器、通知所有会话关闭
            /// @warning    线程安全：关闭动作 post 到 IO 线程执行
            void HttpServer::Stop()
            {
                http_running.store(false);

                auto self = selfWeak.lock();
                if (!self)
                {
                    // 从未启动过：没有挂起的异步操作，直接关掉监听器与定时器即可
                    boost::system::error_code ignored;
                    http_acceptor.close(ignored);
                    cleanup_timer.cancel(ignored);
                    return;
                }

                // 保活投递：确保关闭动作在 IO 线程内执行
                boost::asio::post(ioc,
                                  [this, self]()
                                  {
                                      boost::system::error_code ec;
                                      http_acceptor.close(ec);
                                      cleanup_timer.cancel(ec);

                                      for (auto& session : http_sessions)
                                      {
                                          if (!session)
                                              continue;

                                          // 先断开反向引用，避免会话的 Stop() 在关闭过程中再回调本对象
                                          session->http_server = nullptr;
                                          session->Stop();
                                      }
                                      http_sessions.clear();
                                  });

                // 顺带把基类的 accept / 监控线程收掉（未启动时基类内部会直接返回）
                Net::Server::Server::Stop();
            }
        } // namespace HttpServer
    } // namespace Server
} // namespace Net
