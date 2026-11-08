/// @file        NetHttpClient.h
/// @brief       出站 HTTP 客户端（同步阻塞，用于在业务线程里调第三方接口）
/// @details     支持 http:// 与 https://（TLS 由 OpenSSL 提供），
///              内部自建连接 → 发请求 → 读完响应 → 关闭，一次性用完即走。
///              典型用途：服务端回调 Cloudflare siteverify、内部微服务互调。
/// @author      jyoushitou
/// @date        2026-11-07
/// @copyright   Copyright (c) 2026
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

namespace Net::Client::Http
{
    /// @brief 请求 / 响应头列表（名值对，按顺序写出）
    using Headers = std::vector<std::pair<std::string, std::string>>;

    /// @brief 出站请求参数
    struct Request
    {
        /// @brief 请求方法（"GET" / "POST" / "PUT" / "DELETE" ...）
        std::string method = "GET";

        /// @brief 完整 URL，只支持 http:// 与 https://
        /// @details 例如 https://challenges.cloudflare.com/turnstile/v0/siteverify
        ///          不支持 URL 里的用户名密码、不支持自动跟随 3xx
        std::string url;

        /// @brief 额外请求头
        /// @note  Host / User-Agent / Content-Length 由客户端自动写，无需自己给
        Headers headers;

        /// @brief 请求体（GET 通常为空）
        std::string body;

        /// @brief 整次请求的超时（毫秒），含解析、连接、TLS 握手、收发
        long long timeoutMs = 15000;

        /// @brief https 是否校验对端证书与主机名
        /// @warning 自签证书调试时可以关，生产环境务必保持 true
        bool verifyPeer = true;

        /// @brief CA 证书文件（PEM）；留空则用系统默认路径
        /// @details Windows 上 OpenSSL 的默认路径往往是空的，校验失败时请显式指定，
        ///          例如 vcpkg 的 <vcpkg>/installed/<triplet>/etc/ssl/cacert.pem
        std::string caFile;
    };

    /// @brief 出站响应
    struct Response
    {
        /// @brief HTTP 状态码（如 200 / 404）
        unsigned status = 0;

        /// @brief 响应体（Beast 已按 Content-Length / chunked 解码成明文）
        std::string body;

        /// @brief 响应头
        Headers headers;

        /// @brief 失败原因（人话），成功时为空
        /// @details URL 非法、CA 加载失败、握手失败这类「不是系统错误码能说清」的问题写在这里，
        ///          方便直接打日志
        std::string error;
    };

    /// @brief 出站 HTTP 客户端
    /// @details 同步阻塞实现：调用期间当前线程会一直等结果。
    /// @warning    禁止在 IO 线程（业务回调）里直接调用，会把事件循环卡住；
    ///             请丢到工作线程执行，算完再用 httpAsyncReplay 回包
    class HttpClient
    {
    public:
        /// @brief 构造函数
        /// @param[in] io 用于解析与连接的 io_context（只借用，不 run）
        /// @note  每次 Send 内部自建 socket，用完即关，不做连接复用
        explicit HttpClient(boost::asio::io_context& io);

        /// @brief 发起一次请求（阻塞）
        /// @param[in] req 请求参数
        /// @param[out] ec 错误码；失败时非 0，响应内容无意义
        /// @return 响应（ec 为 0 时有效）
        /// @warning 阻塞调用，禁止在 IO 线程内使用
        /// @note  URL 非法、DNS 失败、连接/TLS/收发超时都会以 ec 返回，不抛异常
        Response Send(const Request& req, boost::system::error_code& ec);

    private:
        boost::asio::io_context& ioc;
    };
} // namespace Net::Client::Http
