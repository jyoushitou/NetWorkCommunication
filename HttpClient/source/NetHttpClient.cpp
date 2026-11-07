/// @file        NetHttpClient.cpp
/// @brief       出站 HTTP 客户端实现（Beast + OpenSSL，同步阻塞）
#include "NetHttpClient.h"

#include <chrono>
#include <cctype>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

namespace Net::Client::Http
{
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    namespace ssl = net::ssl;
    using tcp = net::ip::tcp;
    namespace errc = boost::system::errc;

    namespace
    {
        /// @brief URL 拆解结果
        struct UrlParts
        {
            bool tls = false;        // true = https
            std::string host;        // 不含端口
            std::string port;        // "443" / "80" / 自定义
            std::string target;      // 从 '/' 开始的路径 + query
            std::string hostHeader;  // Host 头（默认端口时省略端口，IPv6 补方括号）
        };

        /// @brief 把 "http(s)://host[:port]/path?query" 拆开
        /// @details 不支持 URL 里的 user:pass@，不支持裸 IPv6 之外的奇怪写法
        /// @return 解析成功返回 true；失败时 why 里带原因
        bool ParseUrl(const std::string& url, UrlParts& out, std::string& why)
        {
            const auto schemeEnd = url.find("://");
            if (schemeEnd == std::string::npos)
            {
                why = "URL 缺少 scheme（必须以 http:// 或 https:// 开头）";
                return false;
            }

            std::string scheme = url.substr(0, schemeEnd);
            for (auto& ch : scheme)
            {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (scheme != "http" && scheme != "https")
            {
                why = "只支持 http / https，当前是 " + scheme;
                return false;
            }
            out.tls = (scheme == "https");

            std::string rest = url.substr(schemeEnd + 3);
            const auto slash = rest.find('/');
            std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
            out.target = (slash == std::string::npos) ? "/" : rest.substr(slash);

            // 去掉 URL 里可能带的 user:pass@
            const auto at = authority.rfind('@');
            if (at != std::string::npos)
            {
                authority = authority.substr(at + 1);
            }
            if (authority.empty())
            {
                why = "URL 缺少主机名";
                return false;
            }

            bool ipv6 = false;
            if (authority.front() == '[')
            {
                const auto close = authority.find(']');
                if (close == std::string::npos)
                {
                    why = "IPv6 地址缺少右方括号";
                    return false;
                }
                out.host = authority.substr(1, close - 1);
                ipv6 = true;
                if (close + 1 < authority.size() && authority[close + 1] == ':')
                {
                    out.port = authority.substr(close + 2);
                }
            }
            else
            {
                const auto colon = authority.rfind(':');
                if (colon == std::string::npos)
                {
                    out.host = authority;
                }
                else
                {
                    out.host = authority.substr(0, colon);
                    out.port = authority.substr(colon + 1);
                }
            }
            if (out.host.empty())
            {
                why = "URL 缺少主机名";
                return false;
            }
            if (out.port.empty())
            {
                out.port = out.tls ? "443" : "80";
            }

            out.hostHeader = ipv6 ? ("[" + out.host + "]") : out.host;
            const bool defaultPort = (out.tls && out.port == "443") || (!out.tls && out.port == "80");
            if (!defaultPort)
            {
                out.hostHeader += ":" + out.port;
            }
            return true;
        }

        /// @brief 在已经连好的流上发请求、读响应（明文与 TLS 共用）
        /// @tparam Stream beast::tcp_stream 或 beast::ssl_stream<beast::tcp_stream>
        /// @return 响应；失败时 ec 非 0，res.error 里是人话
        template <class Stream>
        Response Exchange(Stream& stream, const UrlParts& parts, const Request& req, boost::system::error_code& ec)
        {
            Response res;

            const auto verb = http::string_to_verb(req.method);
            if (verb == http::verb::unknown)
            {
                res.error = "不支持的 HTTP 方法：" + req.method;
                ec = boost::system::error_code(static_cast<int>(errc::invalid_argument), boost::system::generic_category());
                return res;
            }

            http::request<http::string_body> message;
            message.method(verb);
            message.target(parts.target);
            message.version(11);
            message.set(http::field::host, parts.hostHeader);
            message.set(http::field::user_agent, "CommonNetHttpClient/1.0");
            message.set(http::field::accept, "*/*");
            for (const auto& header : req.headers)
            {
                if (!header.first.empty())
                {
                    message.set(header.first, header.second);
                }
            }
            // 有 body 的方法即使 body 为空也显式带上 Content-Length: 0
            const bool bodyMethod = (verb == http::verb::post || verb == http::verb::put || verb == http::verb::patch);
            if (!req.body.empty() || bodyMethod)
            {
                message.body() = req.body;
            }
            message.prepare_payload();

            beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(req.timeoutMs));
            http::write(stream, message, ec);
            if (ec)
            {
                res.error = "发送请求失败：" + ec.message();
                return res;
            }

            beast::flat_buffer buffer;
            http::response<http::string_body> response;
            beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(req.timeoutMs));
            http::read(stream, buffer, response, ec);
            if (ec)
            {
                res.error = "读取响应失败：" + ec.message();
                return res;
            }

            res.status = response.result_int();
            res.body = std::move(response.body());
            for (const auto& field : response.base())
            {
                res.headers.emplace_back(std::string(field.name_string()), std::string(field.value()));
            }
            return res;
        }
    } // namespace

    HttpClient::HttpClient(boost::asio::io_context& io) : ioc(io) {}

    Response HttpClient::Send(const Request& req, boost::system::error_code& ec)
    {
        ec.clear();
        Response res;

        UrlParts parts;
        std::string why;
        if (!ParseUrl(req.url, parts, why))
        {
            res.error = why;
            ec = boost::system::error_code(static_cast<int>(errc::invalid_argument), boost::system::generic_category());
            return res;
        }

        tcp::resolver resolver(ioc);
        const auto results = resolver.resolve(parts.host, parts.port, ec);
        if (ec)
        {
            res.error = "DNS 解析失败：" + parts.host + "（" + ec.message() + "）";
            return res;
        }

        if (parts.tls)
        {
            ssl::context ctx(ssl::context::tls_client);
            boost::system::error_code setupEc;
            if (!req.caFile.empty())
            {
                ctx.load_verify_file(req.caFile, setupEc);
            }
            else
            {
                ctx.set_default_verify_paths(setupEc);
            }

            beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), parts.host.c_str()))
            {
                res.error = "设置 TLS SNI 失败";
                ec = boost::system::error_code(static_cast<int>(errc::io_error), boost::system::generic_category());
                return res;
            }

            if (req.verifyPeer)
            {
                // Windows 上 OpenSSL 的默认证书路径经常是空的：加载失败就直接报给调用方
                if (setupEc)
                {
                    res.error = "加载 CA 证书失败（可用 Request::caFile 指定 PEM）：" + setupEc.message();
                    ec = setupEc;
                    return res;
                }
                stream.set_verify_mode(ssl::verify_peer);
                stream.set_verify_callback(ssl::host_name_verification(parts.host));
            }
            else
            {
                stream.set_verify_mode(ssl::verify_none);
            }

            beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(req.timeoutMs));
            beast::get_lowest_layer(stream).connect(results, ec);
            if (ec)
            {
                res.error = "TCP 连接失败：" + ec.message();
                return res;
            }

            beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(req.timeoutMs));
            stream.handshake(ssl::stream_base::client, ec);
            if (ec)
            {
                res.error = "TLS 握手失败（自签/链不全时检查 caFile 或 verifyPeer）：" + ec.message();
                return res;
            }

            res = Exchange(stream, parts, req, ec);
            boost::system::error_code ignore;
            stream.shutdown(ignore); // 尽力优雅关闭，失败无所谓
            return res;
        }

        beast::tcp_stream stream(ioc);
        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(req.timeoutMs));
        beast::get_lowest_layer(stream).connect(results, ec);
        if (ec)
        {
            res.error = "TCP 连接失败：" + ec.message();
            return res;
        }

        res = Exchange(stream, parts, req, ec);
        boost::system::error_code ignore;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignore);
        return res;
    }
} // namespace Net::Client::Http
