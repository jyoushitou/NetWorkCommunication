// NetHttpServer.cpp
#include "NetHttpServer.h"

#include <algorithm>

namespace Net
{
    namespace Server
    {
        namespace HttpServer
        {
            //========== HttpSession ==========

            // 安全获取自身 unique_ptr（重载返回 HttpSession 类型，避免 protected 访问问题）
            std::shared_ptr<HttpSession> HttpSession::shared_from_this()
            {
                return std::static_pointer_cast<HttpSession>(Session::shared_from_this());
            }

            HttpSession::HttpSession(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock,
                                     std::shared_ptr<VueCallBack> VueCallBackFunction)
                : Net::Server::Session(io, std::move(sock))
            {
                // 创建时立即记录活动时间，防止刚建好就被清理线程误删
                UpdateActiveTime();
            }

            // 更新最近活动时间
            void HttpSession::UpdateActiveTime()
            {
                last_active_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count(),
                                     std::memory_order_relaxed);
            }

            void HttpSession::Start()
            {
                // HTTP/1.1 默认 Keep-Alive：每个请求处理完后可能需要复用本连接继续读下一个请求，
                // 这里把 parser_ 重置为初始状态（重新 make_unique，任何 Boost 版本都兼容）。
                parser_ = std::make_unique<boost::beast::http::request_parser<boost::beast::http::string_body>>();

                // 清空 buffer_ 残留数据（如果上一次读取有剩余字节，会干扰下一次解析）
                buffer_.consume(buffer_.size());

                // 清空url，避免因为之前的url造成错误
                url = {};

                // 读请求前更新活动时间
                UpdateActiveTime();

                // 异步读取 HTTP 请求头
                boost::beast::http::async_read_header(
                    sock, buffer_, *parser_,
                    [self = shared_from_this()](boost::system::error_code ec, std::size_t) mutable
                    {
                        // 客户端在等待下一个请求时断开连接（Keep-Alive 正常收尾），
                        // 这是正常关闭，不是错误，不打印红色日志。
                        if (ec == boost::beast::http::error::end_of_stream)
                        {
                            Utils::Out::Out_Msg("Keep-Alive 等待期间客户端断开连接，正常关闭");
                            self->Stop();
                            return;
                        }

                        // 检查读是否有错误
                        if (ec)
                        {
                            Utils::Out::Out_Err("HTTP 连接读取错误：" + ec.what());
                            self->Stop();
                            return;
                        }

                        // 有请求活动，更新时间戳
                        self->UpdateActiveTime();

                        // 取出已经解析好的 HTTP 请求对象
                        auto& req = self->parser_->get();

                        // 记录请求方是否要求 Keep-Alive（HTTP/1.1 默认 true）
                        self->url.keep_alive = req.keep_alive();

                        // 路由
                        self->Router(std::string(req.method_string()), std::string(req.target()));

                        // 根据 Content-Length 判断是否有 body
                        auto it_cl = req.find(boost::beast::http::field::content_length);
                        // 检查是不是在末尾或者字段为空
                        if (it_cl != req.end() && !it_cl->value().empty())
                        {
                            // 防御性解析：Content-Length 非法时直接返回 400
                            unsigned long long content_length = 0;
                            try
                            {
                                content_length = std::stoull(std::string(it_cl->value()));
                            }
                            catch (const std::exception&)
                            {
                                self->HttpSendResponse("{\"code\":1,\"msg\":\"invalid Content-Length\"}");
                                return;
                            }

                            if (content_length > 0)
                            {
                                self->ReadBody();
                            }
                            else
                            {
                                self->HandleRequest();
                            }
                        }
                        else
                        {
                            self->HandleRequest();
                        }
                    });
            }

            // 读取消息体
            void HttpSession::ReadBody()
            {
                // 读取body
                boost::beast::http::async_read(
                    sock, buffer_, *parser_,
                    [self = shared_from_this()](boost::system::error_code ec, std::size_t) mutable
                    {
                        // 客户端在 body 未读完时断开连接（正常关闭），不是错误
                        if (ec == boost::beast::http::error::end_of_stream)
                        {
                            self->Stop();
                            return;
                        }

                        // 检查读是否有错误
                        if (ec)
                        {
                            Utils::Out::Out_Msg("读取消息有错误，错误码：" + ec.what());
                            // 出现错误，关闭连接
                            self->Stop();
                            return;
                        }

                        // body 读到了，更新活动时间
                        self->UpdateActiveTime();

                        // 取出已经解析好的 HTTP 请求对象
                        auto& req = self->parser_->get();

                        // 记录请求方是否要求 Keep-Alive
                        self->url.keep_alive = req.keep_alive();

                        // 读取到消息体
                        self->url.body = req.body();

                        // 调用回调函数
                        self->HandleRequest();
                    });
            }

            // 处理请求（解析 body 并回复）
            void HttpSession::HandleRequest()
            {
                // 标记忙碌：请求处理期间（包括异步等待微服务返回）不允许被清理线程关闭
                busy.store(true, std::memory_order_relaxed);

                // 更新时间
                UpdateActiveTime();

                // 调用 HttpServer 的处理函数（在这里解析 JSON，可能返回同步响应字符串）
                std::string response = http_server->HandleVueRequest(shared_from_this(), url);
                if (!response.empty())
                {
                    // 同步模式：直接发送响应（发送完成回调里会清除 busy）
                    HttpSendResponse(response);
                }
                // 响应为空 → 异步模式：
                // 不发送、不关闭连接，由 ClientWork 收到微服务结果后
                // 通过同一个 session 调用 AsyncSendResponse(msg) 发送
                // busy 保持 true，直到异步响应发送完成后才清除
            }

            // 线程安全发送响应（任何线程均可调用，通过 post 投递到 HttpServer 的 io_context 线程执行）
            void HttpSession::AsyncSendResponse(const std::string& body)
            {
                auto self = shared_from_this();
                boost::asio::post(ioc, [self, body]() mutable { self->HttpSendResponse(body); });
            }

            // Http请求
            void HttpSession::HttpSendResponse(const std::string& body)
            {
                // 保活
                auto self = shared_from_this();

                // ✅ 用 shared_ptr 管理响应对象，保证 async_write 异步写操作期间对象存活
                auto res = std::make_shared<boost::beast::http::response<boost::beast::http::string_body>>(
                    boost::beast::http::status::ok, 11);

                // 构建报文
                res->set(boost::beast::http::field::server, "CppHttpServer/1.0");
                res->set(boost::beast::http::field::content_type, "application/json");
                // 允许跨域
                res->set("Access-Control-Allow-Origin", "*");
                // 消息体复制到发送里
                res->body() = body;

                // ✅ 使用类成员 url.keep_alive（请求解析时保存），而不是 res->keep_alive()（恒为 true）
                bool keep_alive = url.keep_alive;

                // ✅ 根据请求方是否要求 Keep-Alive 设置 Connection 头
                if (keep_alive)
                {
                    res->set(boost::beast::http::field::connection, "keep-alive");
                }
                else
                {
                    res->set(boost::beast::http::field::connection, "close");
                }

                // 根据 body 的内容自动计算并设置 HTTP 报文的 `Content-Length` 头
                res->prepare_payload();

                Utils::Out::Out_Msg("HttpSendResponse: keep_alive = " + std::string(keep_alive ? "true" : "false") +
                                    ", Connection 头 = " + std::string(res->at(boost::beast::http::field::connection)));

                boost::beast::http::async_write(
                    sock, *res,
                    [this, self, res, keep_alive](boost::system::error_code ec, std::size_t) mutable
                    {
                        // 请求处理完毕（无论成功/失败），清除忙碌标记
                        busy.store(false, std::memory_order_relaxed);
                        // 更新活动时间，防止发完响应后立刻被清理线程误杀
                        UpdateActiveTime();

                        // 判断是否有错误
                        if (ec)
                        {
                            Utils::Out::Out_Err("HTTP 响应发送失败，错误码：" + ec.what());
                            Stop();
                            return;
                        }

                        Utils::Out::Out_Msg("HTTP 响应发送完成，keep_alive = " +
                                            std::string(keep_alive ? "true" : "false"));

                        // 发送成功：
                        if (keep_alive)
                        {
                            // Keep-Alive：重置解析器后继续读取下一个 HTTP 请求
                            Start();
                        }
                        else
                        {
                            // 客户端要求关闭连接
                            Stop();
                        }
                    });
            }

            // 停止函数
            void HttpSession::Stop()
            {
                // 继承自 Session::Stop()，通过 io_context 投递关闭操作
                Net::Server::Session::Stop();

                // 从 HttpServer 的会话列表移除（如果是清理线程关闭的，也会走这里）
                if (http_server)
                    http_server->RemoveSession(shared_from_this());
            }

            // 路由
            void HttpSession::Router(const std::string method, std::string target)
            {
                // 防御性检查：target 太短时 substr(pos) 会抛 out_of_range，必须拦截
                if (target.size() < 5)
                    return;

                // 路由到对应的服务器（保持原有判断语义：从下标 5 开始取 13 个字符比较 "articles"）
                if (target.substr(5, 13) == "articles")
                {
                    url.head.set_serviceid(13);

                    // 有足够字符才做后续解析，避免 substr(14) 越界抛异常
                    if (target.size() >= 14)
                    {
                        if (target.substr(14, target.size()) == "meta")
                        {
                            url.head.set_command(1);
                        }
                        else
                        {
                            url.head.set_command(2);
                            url.body = target.substr(14, target.size());
                        }
                    }
                    else
                    {
                        // 只有 /api/articles 或更短路径
                        return;
                    }
                }
            }

            //========== HttpServer ==========

            // 构造函数
            HttpServer::HttpServer(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep,
                                   unsigned short http_port_)
                : Net::Server::Server(io, ep), http_acceptor(io), http_port(http_port_)
            {
                // 初始化 HTTP acceptor
                boost::asio::ip::tcp::endpoint http_ep(boost::asio::ip::tcp::v4(), http_port);

                // 用端点 `http_ep` 的协议（IPv4 或 IPv6）打开 acceptor。
                http_acceptor.open(http_ep.protocol());
                // 允许端口复用不会因为 TIME_WAIT 状态而报“Address already in use”
                http_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
                // 把 acceptor 绑定到 `http_ep` 对应的 IP 和端口上
                http_acceptor.bind(http_ep);
                // 进入监听状态，内核开始接收连接请求。
                http_acceptor.listen();
            }

            // 启动连接Http
            void HttpServer::StartHttpAccept()
            {
                // 保活
                auto self = shared_from_this();

                // 创建连接socket
                auto sock = std::make_shared<boost::asio::ip::tcp::socket>(ioc);

                // 连接
                http_acceptor.async_accept(*sock,
                                           [this, self, sock](boost::system::error_code ec)
                                           {
                                               // 是否已经在运行状态
                                               if (!running)
                                               {
                                                   Utils::Out::Out_Msg("已经有连接的Session");
                                                   return;
                                               }

                                               if (!ec)
                                               {

                                                   Utils::Out::Out_Msg("开始创建连接");

                                                   // 创建 HTTP 会话
                                                   auto session =
                                                       std::make_shared<HttpSession>(ioc, std::move(*sock), this);

                                                   // 放入Session队列
                                                   http_sessions.push_back(session);

                                                   // 启动Session
                                                   session->Start();

                                                   // 继续接收下一个连接
                                                   StartHttpAccept();
                                               }
                                               else
                                               {
                                                   Utils::Out::Out_Err("HTTP accept 错误: " + ec.what());
                                               }
                                           });
            }

            // 关闭函数
            void HttpServer::Stop()
            {
                // 先关掉 HTTP acceptor
                boost::asio::post(ioc,
                                  [this, self = shared_from_this()]()
                                  {
                                      // 关闭 HTTP acceptor
                                      boost::system::error_code ec;
                                      http_acceptor.close(ec);

                                      // 一次轮询关闭Session连接
                                      for (auto& s : http_sessions)
                                          s->Stop();

                                      // 清空http_Session连接
                                      http_sessions.clear();
                                  });

                // 调用基类 Stop（关 TCP acceptor + 唤醒消息队列）
                Net::Server::Server::Stop();
            }

            // 移除并关闭指定会话（io_context 线程内调用）
            void HttpServer::RemoveSession(std::shared_ptr<HttpSession> session)
            {
                // 通过 post 投递到 io_context 线程执行，避免跨线程改 vector
                auto self = shared_from_this();
                boost::asio::post(ioc,
                                  [this, self, session]() mutable
                                  {
                                      // 从 vector 中查找并移除
                                      auto it = std::find(http_sessions.begin(), http_sessions.end(), session);
                                      if (it != http_sessions.end())
                                      {
                                          Utils::Out::Out_Msg("移除空闲/断开 HTTP Session");
                                          http_sessions.erase(it);
                                      }
                                  });
            }

            // 清理空闲超时的 HTTP 会话
            // 清理线程每隔一段时间调用；内部通过 post 投递到 io_context 线程执行真正的清理
            void HttpServer::CleanupIdleSessions(long long idle_timeout_ms)
            {
                auto self = shared_from_this();
                boost::asio::post(ioc,
                                  [this, self, idle_timeout_ms]() mutable
                                  {
                                      long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                             std::chrono::steady_clock::now().time_since_epoch())
                                                             .count();

                                      size_t before = http_sessions.size();
                                      size_t removed = 0;

                                      // 遍历并清理：超时 + 非忙碌 的会话
                                      for (auto it = http_sessions.begin(); it != http_sessions.end();)
                                      {
                                          auto& s = *it;
                                          // 正在处理请求（如等待微服务返回）的会话不清理
                                          if (s->busy.load(std::memory_order_relaxed))
                                          {
                                              ++it;
                                              continue;
                                          }

                                          long long last = s->last_active_ms.load(std::memory_order_relaxed);
                                          if (now_ms - last > idle_timeout_ms)
                                          {
                                              Utils::Out::Out_Msg("清理空闲超时 HTTP Session，空闲时长 = " +
                                                                  std::to_string(now_ms - last) + " ms");
                                              s->Stop();
                                              it = http_sessions.erase(it);
                                              ++removed;
                                          }
                                          else
                                          {
                                              ++it;
                                          }
                                      }

                                      if (removed > 0)
                                      {
                                          Utils::Out::Out_Msg("清理 HTTP 会话完成：清理前 = " + std::to_string(before) +
                                                              ", 清理 = " + std::to_string(removed) +
                                                              ", 剩余 = " + std::to_string(http_sessions.size()));
                                      }
                                  });
            }

            // 注册回调函数
            void HttpServer::SetHandleVueRequestCallback(VueRequestCallback cb)
            {
                handle_vue_cb = std::move(cb);
            }

            // 回调函数过去
            std::string HttpServer::HandleVueRequest(std::shared_ptr<HttpSession> session,
                                                     Net::Server::HttpServer::Url url)
            {
                // 尝试按 JSON 解析（兼容 {"cmd":"xxx"} 的格式）
                try
                {
                    boost::json::value v = boost::json::parse(url.body);
                    if (v.is_object() && v.as_object().contains("cmd") && v.as_object()["cmd"].is_string())
                    {
                        // 前端发的是 {"cmd":"GetUser"} → 提取 "GetUser"
                        if (v.as_object()["cmd"].as_string() == "GetUser")
                        {
                            url.head.set_command(1);
                            url.body = "";
                        }
                    }
                    else
                    {
                        // 是 JSON 但没有 cmd 字段，直接返回错误
                        return "{\"code\":1,\"msg\":\"missing cmd field\"}";
                    }
                }
                catch (const std::exception&)
                {
                    // 解析失败 → 说明前端发的是裸字符串，body 原样保留，直接透传给业务层
                    return "{\"code\":3,\"msg\":\"recivcemsg error\"}";
                }

                //  把解析后的 url 传给业务层
                if (handle_vue_cb)
                    return handle_vue_cb(session, url);

                return "{\"code\":2,\"msg\":\"business callback not set\"}";
            }
        } // namespace HttpServer
    } // namespace Server
} // namespace Net