// NetHttpServer.cpp
#include "NetHttpServer.h"

namespace Net
{
    namespace Server
    {
        namespace HttpServer
        {
            //========== HttpSession ==========

            // 安全获取自身 shared_ptr（重载返回 HttpSession 类型，避免 protected 访问问题）
            std::shared_ptr<HttpSession> HttpSession::shared_from_this()
            {
                return std::static_pointer_cast<HttpSession>(Session::shared_from_this());
            }

            HttpSession::HttpSession(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock, int serviceID_,
                                     HttpServer* http_server_)
                : Net::Server::Session(io, std::move(sock), serviceID_, http_server_), http_server(http_server_),
                  parser_(boost::beast::http::request_parser<boost::beast::http::string_body>{})
            {
            }

            void HttpSession::Start()
            {
                // 异步读取 HTTP 请求头
                boost::beast::http::async_read_header(
                    sock, buffer_, parser_,
                    [self = shared_from_this()](boost::system::error_code ec, std::size_t) mutable
                    {
                        // 检查读是否有错误
                        if (ec)
                        {
                            Utils::Out_Msg("收到的消息的等待有错误，错误码：" + ec.what(), self->serviceID);
                            self->ActuallyClose();
                            return;
                        }

                        // 取出已经解析好的 HTTP 请求对象
                        auto& req = self->parser_.get();

                        // 从解析出的 HTTP 请求中提取「请求方法」和「请求路径」
                        self->method_ = std::string(req.method_string());
                        self->path_ = std::string(req.target());

                        // 根据 Content-Length 判断是否有 body
                        if (req.body().size() > 0)
                        {
                            self->ReadBody();
                        }
                        else
                        {
                            self->HandleRequest("");
                        }
                    });
            }

            // 读取消息体
            void HttpSession::ReadBody()
            {
                // 继续读取剩余的 body
                boost::beast::http::async_read(
                    sock, buffer_, parser_,
                    [self = shared_from_this()](boost::system::error_code ec, std::size_t) mutable
                    {
                        // 检查读是否有错误
                        if (ec)
                        {
                            Utils::Out_Msg("读取消息有错误，错误码：" + ec.what(), self->serviceID);
                            // 出现错误，关闭连接
                            self->ActuallyClose();
                            return;
                        }

                        // 取出已经解析好的 HTTP 请求对象
                        auto& req = self->parser_.get();

                        // 读取到消息体
                        std::string body = req.body();

                        // 调用回调函数
                        self->HandleRequest(body);
                    });
            }

            // 返回回调
            void HttpSession::HandleRequest(const std::string& body)
            {
                // 调用 HttpServer 的处理函数（在这里解析 JSON）
                std::string response = http_server->HandleVueRequest(path_, body);

                // 发送 HTTP 响应
                HttpSendResponse(response);
            }

            // Http请求
            void HttpSession::HttpSendResponse(const std::string& body)
            {
                // 保活
                auto self = shared_from_this();

                // 创建一个状态码 200 的响应对象，`11` 表示 HTTP 版本 1.1
                boost::beast::http::response<boost::beast::http::string_body> res{boost::beast::http::status::ok, 11};

                // 构建报文
                res.set(boost::beast::http::field::server, "CppHttpServer/1.0");
                res.set(boost::beast::http::field::content_type, "application/json");
                // 允许跨域
                res.set("Access-Control-Allow-Origin", "*");
                // 消息体复制到发送里
                res.body() = body;

                // 根据 body 的内容自动计算并设置 HTTP 报文的 `Content-Length` 头（必要时还会调整
                // `Transfer-Encoding`），让这个响应报文完整合法，可以被客户端正确解析。
                res.prepare_payload();

                boost::beast::http::async_write(sock, res,
                                                [this, self](boost::system::error_code ec, std::size_t)
                                                {
                                                    // 判断是否有错误
                                                    if (ec)
                                                    {
                                                        Utils::Out_Msg("发送消息有错误，错误码：" + ec.what(),
                                                                       self->serviceID);
                                                        ActuallyClose();
                                                        return;
                                                    }
                                                    // 简单处理：发送完关闭连接
                                                    ActuallyClose();
                                                });
            }

            // 停止函数
            void HttpSession::Stop()
            {
                // 继承自 Session::Stop()，通过 io_context 投递关闭操作
                Net::Server::Session::Stop();
            }

            //========== HttpServer ==========

            // 构造函数
            HttpServer::HttpServer(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep, int serviceID_,
                                   unsigned short http_port_)
                : Net::Server::Server(io, ep, serviceID_), http_acceptor(io), http_port(http_port_)
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
                                                   Utils::Out_Msg("已经有连接的Session", serviceID);
                                                   return;
                                               }

                                               if (!ec)
                                               {

                                                   Utils::Out_Msg("开始创建连接", serviceID);

                                                   // 创建 HTTP 会话
                                                   auto session = std::make_shared<HttpSession>(ioc, std::move(*sock),
                                                                                                serviceID, this);

                                                   // 放入Session队列
                                                   http_sessions.push_back(session);

                                                   // 启动Session
                                                   session->Start();

                                                   // 继续接收下一个连接
                                                   StartHttpAccept();
                                               }
                                               else
                                               {
                                                   Utils::Out_Err("HTTP accept 错误: " + ec.what(), serviceID);
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

            // 回调函数
            std::string HttpServer::HandleVueRequest(const std::string& path, const std::string& body)
            {
                Utils::Out_Msg("[Vue请求] path=" + path + ", body=" + body, serviceID);

                // TODO: 在这里解析 JSON 并处理你的业务逻辑
                // 例如：解析 body 中的 JSON，调用对应业务函数，返回 JSON 结果

                // 示例：直接返回成功
                return "{\"code\":0,\"msg\":\"success\",\"data\":\"received\"}";
            }
        } // namespace HttpServer
    } // namespace Server
} // namespace Net