/// @file        NetHttpServer.cpp
/// @brief       HTTP 服务端实现（基于 Boost.Beast，只做报文收发，不含路由）
/// @details     解析请求 → 交给业务回调 → 业务通过会话接口回包。
///              方法、目标（含 query）、请求体全部原样交给业务层，本层不做任何分发。
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// 头文件
#include "NetHttpServer.h"

#include <chrono>
#include <exception>
#include <memory>
#include <string>

#include "Utils.h"

namespace Net::Server::HttpServer
{
    namespace
    {
        /// @brief 构造一个最小的 JSON 错误体
        /// @param[in] msg 错误描述
        /// @return 形如 {"code":1,"msg":"..."} 的字符串
        /// @warning 响应体不能为空串：空串是「异步回包」的约定，不能拿来回空包
        /// @note
        std::string ErrorBody(const char* msg)
        {
            return std::string("{\"code\":1,\"msg\":\"") + msg + "\"}";
        }

        /// @brief 传给基类的"发送队列上限"
        /// @details HTTP 链路不走基类的二进制发送队列（toSend），这个值只是为满足基类构造；
        ///          不要再把 HTTP 的报文上限传进来，两个语义完全不同
        constexpr int kBaseSendQueueUnused = 1024 * 1024 * 10;

    } // namespace

    //========== HttpSession ==========

    /// @brief      构造函数
    /// @details    接管 socket，并把空闲超时与发送上限交给基类；随后记一次活动时间
    /// @param[in] io 存储 io_context
    /// @param[in] sock 已 accept 的连接 socket（内部 move 给基类）
    /// @param[in] HF TCP 侧回调（本模块不使用，仅为与 Session 的构造签名保持对称）
    /// @param[in] timeout 空闲超时时间（秒，传 0 时基类回退为 60）
    /// @param[in] maxSendBufLen 发送队列积压上限（HTTP 不使用基类发送队列）
    /// @param[in] maxHttpSendBufLen 请求头 / 请求体的长度上限
    /// @warning    构造完成后必须马上交给 shared_ptr 持有，否则 start() 里的
    ///             shared_from_this() 会抛 std::bad_weak_ptr
    /// @note       基类拿到的是空回调：HTTP 会话绝不能走基类「读 12 字节二进制头」的链路
    HttpSession::HttpSession(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock,
                             std::shared_ptr<HttpHandleFunction> HF, long long timeout, int maxSendBufLen,
                             int maxHttpHeaderLen, int maxHttpSendBufLen)
        : Net::Server::Session(io, std::move(sock), nullptr, timeout, maxSendBufLen)
    {
        // 请求头 / 请求体上限：非正数回退为默认 10MB
        if (maxHttpSendBufLen <= 0)
        {
            Utils::Out::outErr("maxHttpSendBufLen 非法，已回退为默认1024*1024*10");
        }
        this->maxHttpSendBufLen = (maxHttpSendBufLen > 0) ? maxHttpSendBufLen : 1024 * 1024 * 10;

        // 请求头上限（非正数回退为 8KB）：原来这里漏了赋值，header_limit 一直用成员默认值
        this->maxHttpHeadLen = (maxHttpHeaderLen > 0) ? maxHttpHeaderLen : 1024 * 8;

        // 业务回调：原来漏了这一行，导致 HHF 恒为 nullptr，所有请求都回 500 "handler not set"
        this->HHF = HF;

        // 创建时立即记录活动时间，防止刚建好就被巡检误判为空闲
        updateTime();
    }

    /// @brief      开始（或继续）读取下一条 HTTP 请求
    /// @details    重建解析器 → 清空请求上下文 → 异步读请求头；
    ///             读完头后按 parser 的状态决定是直接进业务还是继续读 body
    /// @note       重写基类虚函数，不走基类「读 12 字节二进制头」的链路
    void HttpSession::start()
    {
        // 已经进入关闭流程：不再挂新的读，等巡检线程按 isClosed() 回收
        if (isClosed())
        {
            return;
        }

        // HTTP/1.1 默认 Keep-Alive：每条请求处理完后可能要复用本连接继续读下一条，
        // 这里把解析器重置为初始状态（重新 make_unique，任何 Boost 版本都兼容）
        parser = std::make_unique<boost::beast::http::request_parser<boost::beast::http::string_body>>();
        parser->body_limit(maxHttpSendBufLen); // 请求体上限，超了会得到 body_limit 错误
        parser->header_limit(maxHttpHeadLen);  // 请求头上限，超了会得到 header_limit 错误

        // 清空上一条请求的上下文
        // buffer 不能清：里面可能已经缓存了下一条请求的字节（HTTP pipelining）
        url = Url{};

        // 请求号：++ 返回自增后的值（1、2、3…），必须与 httpAsyncReplay 里的比较口径一致
        // （原来是 fetch_add(1) 返回旧值 → 每条异步回包都被判成"序号不匹配"）
        url.seq = ++reqSeq;

        // 新请求开始：复位忙标记、「已回包」标记与「回完就关」标记
        busy.store(false, std::memory_order_relaxed);
        busySince.store(0);
        replied.store(false, std::memory_order_relaxed);
        closeAfterReply.store(false, std::memory_order_relaxed);

        // 更新时间
        updateTime();

        // 保活：异步回调执行期间会话不能被析构
        auto self = getSession();

        // 两段式读取的第一段：只读到请求头结束，此时 body 还没有读
        boost::beast::http::async_read_header(
            socket, buffer, *parser,
            [self](boost::system::error_code ec, std::size_t)
            {
                // 期间已经进入关闭流程（空闲超时 / 服务器
                // stop）：结果不再有意义
                if (self->isClosed())
                {
                    return;
                }

                // 对端在 keep-alive 空闲期正常关闭，不是错误
                if (ec == boost::beast::http::error::end_of_stream)
                {
                    self->closeSession();
                    return;
                }

                // 主动关闭 socket 引发的取消：正常退出流程的一部分，静默返回
                if (ec == boost::asio::error::operation_aborted)
                {
                    return;
                }

                // 连接错误 / 报文非法 / 头过大：流已经不可用，只能关连接
                if (ec)
                {
                    Utils::Out::outErr("HTTP 读取请求头失败：" + ec.message());
                    self->errCloseSession();
                    return;
                }

                // 把请求头里的信息落到上下文里（业务层只认 Url）
                // 注意：method_string() / target() 返回 beast 的
                // string_view， 不能直接拿去构造 std::string（不同 Boost
                // 版本不保证可转换）， 统一用 assign(指针, 长度) 复制
                auto& req = self->parser->get();
                const auto methodView = req.method_string();
                const auto targetView = req.target();
                self->url.method.assign(methodView.data(), methodView.size());
                self->url.target.assign(targetView.data(), targetView.size());
                self->url.version = req.version();
                self->url.keep_alive = req.keep_alive();
                self->updateTime();

                // 没有 body（GET、Content-Length: 0）→ 直接进业务
                if (self->parser->is_done())
                {
                    self->handleToWork();
                    return;
                }

                if (req[boost::beast::http::field::expect] == "100-continue")
                {
                    auto cont = std::make_shared<boost::beast::http::response<boost::beast::http::empty_body>>(
                        boost::beast::http::status::continue_, self->url.version ? self->url.version : 11);
                    boost::beast::http::async_write(self->socket, *cont,
                                                    [self, cont](boost::system::error_code wec, std::size_t)
                                                    {
                                                        if (wec || self->isClosed())
                                                        {
                                                            return;
                                                        }
                                                        self->readBody();
                                                    });
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
                                           // 期间已经进入关闭流程：结果不再有意义
                                           if (self->isClosed())
                                           {
                                               return;
                                           }

                                           // 请求体超过 parser->body_limit()：剩余 body 还在流里，只能回 413
                                           // 后断开 （parser->is_done() 为 false，httpReplay 会自动不复用连接）
                                           if (ec == boost::beast::http::error::body_limit)
                                           {
                                               Utils::Out::outErr("HTTP 请求体超过上限，返回 413");
                                               self->httpReplay(ErrorBody("payload too large"),
                                                                boost::beast::http::status::payload_too_large);
                                               return;
                                           }

                                           // 对端在发 body 的过程中正常关闭
                                           if (ec == boost::beast::http::error::end_of_stream)
                                           {
                                               self->closeSession();
                                               return;
                                           }

                                           // 主动关闭 socket 引发的取消：正常退出流程的一部分，静默返回
                                           if (ec == boost::asio::error::operation_aborted)
                                           {
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
    /// @details    置忙标记后调用业务回调；回调返回后不做任何事：
    ///             已回包的，由业务那次投递完成发送；还没回包的，保持忙状态等业务层补发
    /// @warning    回调在 IO 线程内执行，业务里禁止阻塞
    void HttpSession::handleToWork()
    {
        // 标记忙碌：请求处理期间（包括等待业务异步回包）不允许被巡检线程关掉
        busy.store(true, std::memory_order_relaxed);
        busySince.store(Utils::Time::nowTime());   // 巡检靠它判断"忙了多久"
        updateTime();

        // 回调没设置：不能把连接挂死在这里，直接回 500
        if (HHF == nullptr)
        {
            Utils::Out::outErr("未设置 HTTP 业务回调函数，返回 500");
            httpReplay(ErrorBody("handler not set"), boost::beast::http::status::internal_server_error);
            return;
        }

        try
        {
            // 回调内部若调用 httpAsyncReplay，投递的任务会在本函数返回后立即执行
            (*HHF)(getSession(), url);
        }
        catch (const std::exception& e)
        {
            Utils::Out::outErr("HTTP 业务回调抛出异常：" + std::string(e.what()));
            // 若业务已经先回过一次包，httpReplay 内部的 replied 判定会丢弃这次重复响应
            httpReplay(ErrorBody("internal error"), boost::beast::http::status::internal_server_error);
        }
        catch (...)
        {
            Utils::Out::outErr("HTTP 业务回调抛出未知异常");
            httpReplay(ErrorBody("internal error"), boost::beast::http::status::internal_server_error);
        }

        // 回调返回后这里不再做任何事：
        //   · 业务已经在回调内调用过 httpAsyncReplay → 那次投递会紧接着执行并完成发送
        //   · 业务还没回包 → busy 保持 true，等业务层稍后用 httpAsyncReplay 补发
        //     （巡检线程不会按空闲超时杀掉忙会话）
    }

    /// @brief      发送 HTTP 响应报文
    /// @details    组报文 → 异步写 → 写完按 keep-alive 决定复用连接还是关闭
    /// @param[in] body 响应体（非空）
    /// @param[in] st 状态码
    /// @return     报文已交给 socket 返回 true；本条请求已经回过包、本次被丢弃返回 false
    /// @warning    每条请求只回一次包：重复调用会把两条响应拼到同一条流上，报文会直接乱掉
    bool HttpSession::httpReplay(const std::string& body, boost::beast::http::status st)
    {
        if (isClosed())
        {
            Utils::Out::outErr("会话关闭中，此次发送不发送");
            return false;
        }

        // 一条请求只允许写出一条响应报文
        if (replied.exchange(true))
        {
            Utils::Out::outErr("本次 HTTP 请求已经回过包，忽略重复响应");
            return false;
        }

        // 只有请求体读完才允许复用连接：提前拒绝（413）时流里还有未读数据，必须关
        const bool keep =
            url.keep_alive && parser && parser->is_done() && !closeAfterReply.load(std::memory_order_relaxed);

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

        // 保活：报文对象与自身都要活到写完成
        auto self = getSession();

        boost::beast::http::async_write(socket, *res,
                                        [self, res](boost::system::error_code ec, std::size_t)
                                        {
                                            // 无论成功还是失败，这条请求到这里都处理完了
                                            self->busy.store(false, std::memory_order_relaxed);
                                            self->busySince.store(0);
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

                                            // 关闭流程可能在本响应写出期间被触发（空闲巡检 / 服务器 stop）
                                            if (self->isClosed())
                                            {
                                                return;
                                            }

                                            // 复用连接：继续读下一条请求
                                            self->start();
                                        });

        // 报文已经交给 socket，剩下的由写完成回调推进
        return true;
    }

    /// @brief      线程安全地回复响应
    /// @details    任意线程均可调用，内部 post 到 IO 线程执行，避免跨线程碰 socket / parser
    /// @param[in] body 响应体（空串会被忽略）
    /// @warning    会话已关闭、或当前没有请求在等回包时，本次调用被丢弃
    /// @note
    void HttpSession::httpAsyncReplay(unsigned long long seq, const std::string& body, boost::beast::http::status st)
    {
        // 空响应体没有意义，直接丢弃（约定了空串表示「异步」，不能拿来回空包）
        if (body.empty())
        {
            Utils::Out::outErr("HTTP 异步回包内容为空，已丢弃");
            return;
        }

        auto self = getSession();

        boost::asio::post(socket.get_executor(),
                          [self, seq, body, st]()
                          {
                              // 会话已关闭（超时、对端断开、服务器停止）→ 丢弃
                              if (self->isClosed())
                              {
                                  return;
                              }

                              // 关键：迟到/重复的回包不能拿去答下一条请求
                              if (seq != self->reqSeq.load(std::memory_order_relaxed))
                              {
                                  Utils::Out::outErr("HTTP 异步回包序号不匹配（迟到回包），已丢弃");
                                  return;
                              }

                              // 当前没有请求在等回包，或这条请求已经回过包 → 丢弃，避免重复响应
                              if (!self->busy.load(std::memory_order_relaxed) ||
                                  self->replied.load(std::memory_order_relaxed))
                              {
                                  return;
                              }

                              self->httpReplay(body, st);
                          });
    }

    /// @brief      兼容接口：按"当前请求"回包
    /// @details    等价于 httpAsyncReplay(reqSeq, body, 200)，供旧代码/文档里的单参写法使用
    /// @warning    迟到回包可能被当成下一条请求的响应，新代码请用带 seq 的重载
    /// @note
    void HttpSession::httpAsyncReplay(const std::string& body)
    {
        httpAsyncReplay(reqSeq.load(std::memory_order_relaxed), body, boost::beast::http::status::ok);
    }

    /// @brief  标记本次响应发完就关闭连接
    /// @details 空闲巡检给超时会话回 408 时使用：既把提示发出去，又保证回完就断开
    /// @note   线程安全（原子标志）
    void HttpSession::markCloseAfterReply()
    {
        closeAfterReply.store(true, std::memory_order_relaxed);
    }

    /// @brief 获得自身指针
    /// @return 返回自己的共享指针（static_pointer_cast 不做运行时校验，正常不会为空）
    /// @warning 仅在对象由 shared_ptr 持有时可用
    /// @note
    std::shared_ptr<HttpSession> HttpSession::getSession()
    {
        return std::static_pointer_cast<HttpSession>(shared_from_this());
    }

    /// @brief 正常的关闭连接
    /// @details 走基类的优雅关闭；会话表里的清理由服务器的巡检线程按 isClosed() 回收
    void HttpSession::closeSession()
    {
        // 基类：置停止标志 + 优雅关闭（socket 关掉时回调 derivedToClose）
        Session::closeSession();
    }

    /// @brief 出错关闭的实际执行体
    void HttpSession::closeWithError()
    {
        Session::errToClose();
    }

    /// @brief 错误的关闭连接
    /// @details 走基类的出错关闭：直接清空并关闭，不等队列发完
    void HttpSession::errCloseSession()
    {
        auto self = getSession();
        boost::asio::post(socket.get_executor(), [self]() { self->closeWithError(); });
    }

    //========== HttpServer ==========

    /// @brief      构造函数
    /// @details    初始化 HTTP 监听器（open / reuse_address / bind / listen）
    /// @param[in] io 存储 io_context
    /// @param[in] ep 监听地址，只有地址部分生效
    /// @param[in] epHttp http的监听端点，HTTP 端口由 epHttp 决定
    /// @warning    bind / listen 失败会抛 boost::system::system_error（与基类一致）
    /// @note       传给基类的端点端口被改成 0（系统分配临时端口）：基类的 acceptor
    ///             本模块不使用，这样即使 ep 的端口与 http 端口相同也不会冲突
    HttpServer::HttpServer(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep,
                           boost::asio::ip::tcp::endpoint epHttp, std::shared_ptr<HttpHandleFunction> HHF,
                           std::shared_ptr<HandleFunction> HF, long long timeOut, long long httpTimeOut,
                           long long checkTime, long long httpCheckTime, int maxSendBufLen, int maxHttpSendBufLen,
                           long long sessionTimeOut, long long httpSessionTimeOut, int maxHttpHeaderLen,
                           int maxConnections)
        : Server(io, boost::asio::ip::tcp::endpoint(ep.address(), 0), HF, timeOut, checkTime, maxSendBufLen,
                 sessionTimeOut),
          epHttp(epHttp),
          httpAcceptor(io)
    {
        // 打开 HTTP 监听器（端口由 epHttp 决定；基类的 acceptor 只是占位，绑在端口 0 上）
        boost::system::error_code openEc;
        if (!openHttpAcceptor(openEc))
        {
            throw boost::system::system_error(openEc, "HttpServer: 打开监听端口失败");
        }

        // 设置HHF回调
        this->HHF = HHF;

        // 判断是否设置了业务回调
        if (this->HHF == nullptr)
        {
            Utils::Out::outErr("未设置 HTTP 业务回调函数，收到的请求将统一返回 500");
        }

        // 设置超时时间
        this->httpTimeOut = (httpTimeOut > 0) ? httpTimeOut : 60;

        // 设置巡检时间
        this->httpCheckTime = (httpCheckTime > 0) ? httpCheckTime : 60;

        // 设置会话忙时最多超时挨踢时间（非正数回退为 60 秒）
        if (httpSessionTimeOut <= 0)
        {
            Utils::Out::outErr("httpSessionTimeOut 非法（" + std::to_string(httpSessionTimeOut) +
                               "），已回退为默认 60 秒");
        }
        this->httpSessionTimeOut = (httpSessionTimeOut > 0) ? httpSessionTimeOut : 60;

        // 判断httpTimeOut、httpCheckTime是否合法
        if (httpTimeOut <= 0 || httpCheckTime <= 0)
        {
            Utils::Out::outErr("httpTimeOut / httpCheckTime 非法，已回退为默认 60 秒");
        }

        // 初始化每个 session 的请求头 / 请求体上限
        this->maxHttpSendBufLen = (maxHttpSendBufLen > 0) ? maxHttpSendBufLen : 1024 * 1024 * 10;
        this->maxHttpHeaderLen = (maxHttpHeaderLen > 0) ? maxHttpHeaderLen : 1024 * 8;

        // 最大并发连接数（非正数视为不限制）
        this->maxConnections = maxConnections;

        // 判断maxHttpSendBufLen是否合法
        if (maxHttpSendBufLen <= 0)
        {
            Utils::Out::outErr("maxHttpSendBufLen 非法，已回退为默认1024*1024*10");
        }

        httpRunning.store(false);

        httpAcceptorConnectionCount = 0;

        Utils::Out::outMsg("HTTP 服务器初始化完成");
    }

    /// @brief      打开 HTTP 监听器
    /// @details    open / reuse_address / bind / listen；构造与 start() 共用
    /// @param[out] ec 失败原因
    /// @return      已打开（或本次打开成功）返回 true
    /// @note        幂等：已经打开时直接返回 true，不重复 bind
    bool HttpServer::openHttpAcceptor(boost::system::error_code& ec)
    {
        if (httpAcceptor.is_open())
        {
            ec.clear();
            return true;
        }

        httpAcceptor.open(epHttp.protocol(), ec);
        if (ec)
        {
            return false;
        }

        httpAcceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
        if (ec)
        {
            return false;
        }

        httpAcceptor.bind(epHttp, ec);
        if (ec)
        {
            return false;
        }

        httpAcceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec)
        {
            return false;
        }

        return true;
    }

    /// @brief      析构函数
    /// @details    先 stop() 停服并关闭监听器与全部会话，再 join 巡检线程
    /// @note       基类 Server 的析构随后自动执行（其中会再调一次基类 stop()，幂等且无副作用）
    HttpServer::~HttpServer()
    {
        // 判断session现在连接数，不影响正常析构
        if (!httpSessions.empty())
        {
            Utils::Out::outErr("HttpSessions仍有" + std::to_string(httpSessions.size()) + "个会话未关闭");
        }

        // 先触发停止：置 httpRunning=false 并关闭 acceptor / 会话
        // 析构阶段 selfWeak 已经 lock 不到指针，stop() 内部会走同步关闭分支
        stop();

        // 回收巡检线程（stop() 已置标志并 notify，线程会自行退出循环）
        if (httpClearSessionThread.joinable())
        {
            httpClearSessionThread.join();
        }
    }

    /// @brief 启动
    /// @details 置 httpRunning、绑定 selfWeak（巡检线程与 stop() 靠它保活），
    ///          随后启动巡检线程并开始 accept
    /// @warning 会启动一个巡检线程；调用者必须已经用 shared_ptr 持有本对象
    /// @note 幂等：已在运行时只打印提示；stop() 后重启会先 join 掉上一轮的巡检线程
    void HttpServer::start()
    {
        // 判断是否已经启动（幂等：已在运行时只打印提示）
        if (httpRunning.load())
        {
            Utils::Out::outMsg("HTTP 服务器已经在运行监听了");
            return;
        }

        // stop() 后重启：先回收上一轮的巡检线程。
        // 给 joinable 的 std::thread 赋值会直接 std::terminate()，在这里兜住。
        if (httpClearSessionThread.joinable())
        {
            httpClearSessionThread.join();
        }

        // 重新打开 HTTP 监听器（stop() 关过它，这里重开才支持重启）
        boost::system::error_code ec;
        if (!openHttpAcceptor(ec))
        {
            Utils::Out::outErr("HTTP 监听失败：" + ec.message());
            throw boost::system::system_error(ec, "HttpServer监听失败");
        }

        // 将运行状态置为true
        httpRunning.store(true);

        try
        {
            // 基类的 selfWeak 在这里指向本对象（Server 继承自 enable_shared_from_this<Server>，
            // 这里向下转型后存入基类的 weak_ptr，巡检线程与 stop() 都靠它安全保活）
            selfWeak = std::static_pointer_cast<HttpServer>(shared_from_this());

            // 创建巡检线程
            httpClearSessionThread = std::thread(&HttpServer::clearHttpSession, this);

            // 开始接受连接
            startAccept();
        }
        catch (...)
        {
            // 置为false
            httpRunning.store(false);
            // 唤醒监控线程
            queueHttpCV.notify_all();
            if (httpClearSessionThread.joinable())
            {
                httpClearSessionThread.join();
            }

            // 继续向上抛出异常
            throw;
        }
    }

    /// @brief      开始接受连接
    /// @details    在 io_context 线程中被调用，异步等待并接受客户端连接，
    ///             为每个新连接创建对应的 HttpSession（关闭 NoDelay）后继续接受下一个连接
    /// @warning    须在 io_context 运行后调用；acceptor 被关闭导致的取消错误会静默忽略
    /// @note       accept 失败时按指数退避重试（上限 5s），服务器已停止则不再重试
    void HttpServer::startAccept()
    {
        // 保活 HttpServer 自身（基类 selfWeak 里存的正是本对象）
        auto self = std::static_pointer_cast<HttpServer>(selfWeak.lock());

        if (!self || !httpRunning.load())
        {
            return;
        }

        // 创建一个 socket 用于 accept
        auto sock = std::make_shared<boost::asio::ip::tcp::socket>(ioc);

        // 异步等待并接受新连接
        httpAcceptor.async_accept(
            *sock,
            [this, self, sock](boost::system::error_code ec)
            {
                // 服务器已停止：acceptor 被主动关闭导致的取消错误是正常退出流程的一部分
                if (!httpRunning.load())
                {
                    return;
                }

                if (ec)
                {
                    // 主动关闭监听器引发的取消：正常退出，不必刷错误日志
                    if (ec != boost::asio::error::operation_aborted)
                    {
                        if (ec == boost::asio::error::bad_descriptor)
                        {
                            Utils::Out::outErr("HTTP 监听器已失效，停止 accept：" + ec.message());
                            {
                                std::lock_guard<std::mutex> lock(queueHttpMutex);
                                httpRunning.store(false);
                            }
                            // 唤醒监控线程
                            queueHttpCV.notify_all();
                            return;
                        }
                        Utils::Out::outErr("HTTP accept 错误: " + ec.message());
                    }

                    // 记录重试次数
                    httpAcceptorConnectionCount++;
                    const unsigned shift = (std::min)(httpAcceptorConnectionCount, 6u);
                    const long long delayMs = (std::min)(5000LL, 100LL << shift);

                    // 按退避时长重试（100ms → 200ms → … 上限 5s），避免出错时空转把 CPU 打满
                    auto timer = std::make_shared<boost::asio::steady_timer>(ioc, std::chrono::milliseconds(delayMs));
                    timer->async_wait(
                        [this, self, timer](const boost::system::error_code&)
                        {
                            if (httpRunning.load())
                            {
                                startAccept();
                            }
                        });
                    return;
                }

                // 连接数上限：达到上限就立刻关掉这条新连接，避免句柄/内存被吃光
                if (maxConnections > 0 && httpSessions.size() >= static_cast<std::size_t>(maxConnections))
                {
                    Utils::Out::outErr("HTTP 连接数已达上限 " + std::to_string(maxConnections) + "，拒绝新连接");
                    boost::system::error_code ignored;
                    sock->close(ignored);
                    startAccept();
                    return;
                }

                // HttpSession 继承自 enable_shared_from_this，必须用 shared_ptr 管理
                // 构造函数按值接收 socket，需 std::move(*sock)；
                // 第 5 个参数是基类的发送队列上限（HTTP 不使用，用具名常量标明语义）
                auto session = std::make_shared<HttpSession>(ioc, std::move(*sock), HHF, httpTimeOut,
                                                            kBaseSendQueueUnused, maxHttpHeaderLen, maxHttpSendBufLen);
                // 开启 TCP_NODELAY（禁用 Nagle，小包立即发送）
                session->offNoDelay();
                // 将session放入会话表，由巡检线程统一回收
                httpSessions.push_back(session);
                // 启动 HTTP 读循环
                session->start();

                // 成功一次就把退避计数清零
                httpAcceptorConnectionCount = 0;
                // 继续接受下一个连接
                startAccept();
            });
    }

    /// @brief      巡检线程主体
    /// @details    按 httpCheckTime 周期睡眠（可被 stop() 立即唤醒），醒来后 post 到 io_context：
    ///             对超时会话发送提示并关闭，随后移除已关闭的会话
    /// @warning    在独立线程中运行，禁止直接访问只在 IO 线程内读写的 httpSessions
    /// @note       每轮都通过 selfWeak 取保活指针，取不到说明 HttpServer 已析构，直接退出
    void HttpServer::clearHttpSession()
    {
        // 服务器运行期间循环巡检（httpRunning=false 后退出循环）
        while (httpRunning.load())
        {
            {
                // 创建锁变量
                std::unique_lock<std::mutex> lock(queueHttpMutex);

                // 拿到锁后再确认一次运行状态
                if (!httpRunning.load())
                {
                    break;
                }

                // 先在锁内等一轮；被 stop() 唤醒时立即退出，不必等满周期
                queueHttpCV.wait_for(lock, std::chrono::seconds(httpCheckTime),
                                     [this]() { return !httpRunning.load(); });
                if (!httpRunning.load())
                {
                    break;
                }
            }

            // 获取其保活指针
            auto self = std::static_pointer_cast<HttpServer>(selfWeak.lock());
            if (!self)
            {
                return;
            }

            // post 到 io_context，在 IO 线程内清理失效会话（避免跨线程直接改 httpSessions）
            boost::asio::post(ioc,
                              [this, self]()
                              {
                                  // 逐个会话检查
                                  for (auto& it : httpSessions)
                                  {
                                      if (!it)
                                      {
                                          continue;
                                      }

                                      // 正在等业务回包的会话不能按空闲超时杀掉：业务可能只是慢但是不能无限期等待
                                      if (it->busing())
                                      {
                                          if (it->busyTimedOut(httpSessionTimeOut))
                                          {
                                              Utils::Out::outErr("HTTP 会话等待业务回包超时，返回503");
                                              it->markCloseAfterReply();
                                              if (!it->httpReplay(ErrorBody("busing timeout"),
                                                                  boost::beast::http::status::service_unavailable))
                                              {
                                                  it->closeSession();
                                              }
                                          }
                                          continue;
                                      }

                                      if (!it->timeOut())
                                      {
                                          continue;
                                      }

                                      try
                                      {
                                          // 标记「回完就关」，再回一条 408（写出完成后由回调关闭连接）
                                          it->markCloseAfterReply();

                                          // 回不出去（上一条响应还卡在 socket 里没写完）→ 直接断，
                                          // 否则这条连接会永远留在会话表里
                                          if (!it->httpReplay(ErrorBody("request timeout"),
                                                              boost::beast::http::status::request_timeout))
                                          {
                                              it->closeSession();
                                          }
                                      }
                                      catch (const std::exception& e)
                                      {
                                          Utils::Out::outErr("HTTP 空闲超时回包失败：" + std::string(e.what()));
                                          it->closeSession();
                                      }
                                      catch (...)
                                      {
                                          Utils::Out::outErr("HTTP 空闲超时回包出现未知错误");
                                          it->closeSession();
                                      }
                                  }

                                  // 检查是否关闭
                                  for (auto iter = httpSessions.begin(); iter != httpSessions.end();)
                                  {
                                      if (!(*iter) || (*iter)->isClosed())
                                      {
                                          // 避免悬空的迭代器
                                          iter = httpSessions.erase(iter);
                                          // 避免后面自增
                                          continue;
                                      }
                                      ++iter;
                                  }
                              });
        }
    }

    /// @brief      停止服务
    /// @details    置 httpRunning=false 并 notify_all 唤醒巡检线程，再 post 到 IO 线程关闭监听器与全部会话
    /// @warning    调用后服务器不再接受新连接，须重新构造使用
    /// @note       幂等，可重复调用；HttpServer 尚未 start()（selfWeak 为空）或正在析构时直接同步关闭
    void HttpServer::stop()
    {
        {
            // 加锁
            std::lock_guard<std::mutex> lock(queueHttpMutex);
            // 无条件置停止标志（幂等；这里绝不做任何"闸门"早退，否则关闭动作会被跳过）
            httpRunning = false;
        }

        // 唤醒正在周期等待的巡检线程，让它立即退出等待
        queueHttpCV.notify_all();

        // 保活，不保存引用
        auto self = std::static_pointer_cast<HttpServer>(selfWeak.lock());

        // 从未启动过 / 正在析构 / IO 线程已停：投递出去也没人执行 → 就地同步关闭
        if (!self || ioc.stopped())
        {
            closeHttpAll();
            return;
        }

        // post 到 IO 线程执行关闭动作（关的是 HTTP 自己的监听器与会话表）
        boost::asio::post(ioc, [this, self]() { closeHttpAll(); });
    }

    /// @brief 关闭监听器与全部会话
    /// @details 关闭 acceptor 后逐个通知会话关闭，最后清空会话表
    /// @warning 只能在 IO 线程内调用（析构阶段除外：那时 IO 线程已经不再运行）
    /// @note 幂等
    void HttpServer::closeHttpAll()
    {
        // 关闭 acceptor，不再接受新连接
        boost::system::error_code ec;
        httpAcceptor.close(ec);

        // 循环通知每个会话关闭（closeSession 内部是投递关闭，本身线程安全）
        for (auto& session : httpSessions)
        {
            if (session)
            {
                session->closeSession();
            }
        }

        // 清理会话
        httpSessions.clear();
    }
} // namespace Net::Server::HttpServer
