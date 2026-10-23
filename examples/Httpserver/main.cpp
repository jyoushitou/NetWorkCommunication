/// @file        main.cpp
/// @brief       HTTP 服务端示例入口（启动 HTTP 服务、演示同步与异步两种回包）
/// @author      jyoushitou
/// @date        2026-10-23
/// @copyright   Copyright (c) 2026

// examples/Httpserver/main.cpp
#include "NetHttpServer.h"
#include "Utils.h"
#include "Message.h"

#include <boost/asio.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

/// @brief HTTP 监听端口
constexpr unsigned short HttpPort = 27080;

/// @brief 服务 ID 的存放位置说明
/// @details serviceID 由 vcpkg 的 utils 包（Utils.dll）定义并导出，使用方只需在运行时赋值，
///          不要在消费侧再给定义（会与 Utils.dll 冲突）。
/// @note 见 main() 里的 Utils::serviceID = RPCGateway;

int main()
{
    // 控制台 / 日志目录 / 退出事件与信号处理（含 Ctrl+C、关闭窗口）
    Utils::init();

    // 设置当前服务 ID（决定日志里的服务名）
    Utils::serviceID = RPCGateway;

    boost::asio::io_context io;

    // 基类 Server 的端点是"占位"的：HttpServer 自己用 epHttp 监听，基类 acceptor 不会被打开，
    // 这里传端口 0 只是为了满足构造签名
    const boost::asio::ip::tcp::endpoint epTcp(boost::asio::ip::tcp::v4(), 0);
    const boost::asio::ip::tcp::endpoint epHttp(boost::asio::ip::tcp::v4(), HttpPort);

    // 业务回调：在 IO 线程内执行，禁止长时间阻塞
    //  - 普通路径：同步回包（httpReplay）
    //  - /slow  路径：切到工作线程算，算完后用 httpAsyncReplay(seq, body) 异步回包
    //    （seq 必须用回调给的这个请求号，否则迟到的回包会被当成下一条请求的响应而丢弃）
    auto hhf = std::make_shared<Net::Server::HttpServer::HttpHandleFunction>(
        [](const std::shared_ptr<Net::Server::HttpServer::HttpSession>& session,
           const Net::Server::HttpServer::Url& url)
        {
            Utils::Out::outMsg("收到请求：" + url.method + " " + url.target);

            if (url.target == "/slow")
            {
                // 模拟慢业务：不在 IO 线程里阻塞，丢给工作线程后再回包
                auto seq = url.seq;
                std::thread([session, seq]()
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                                // 异步回包接口线程安全；会话已关闭时会被内部丢弃
                                session->httpAsyncReplay(seq, "{\"code\":0,\"msg\":\"slow ok\"}",
                                                         boost::beast::http::status::ok);
                            })
                    .detach();
                return;
            }

            // 同步回包：默认 200；想改状态码就传第二个参数，例如 http::status::not_found
            const std::string body = "{\"code\":0,\"msg\":\"ok\",\"target\":\"" + url.target + "\"}";
            session->httpReplay(body);
        });

    // 创建 HTTP 服务器（其余参数：TCP 侧回调、各类超时、连接数上限等都走默认值）
    std::shared_ptr<Net::Server::HttpServer::HttpServer> server;
    try
    {
        server = std::make_shared<Net::Server::HttpServer::HttpServer>(io, epTcp, epHttp, hhf);
    }
    catch (const std::exception& e)
    {
        Utils::Out::outErr(std::string("HTTP 服务器创建失败（端口 ") + std::to_string(HttpPort) +
                           " 可能已被占用）：" + e.what());
        return -1;
    }

    // 退出回调：用 weak_ptr 捕获，避免全局回调表让 server 活过 main
    std::weak_ptr<Net::Server::HttpServer::HttpServer> weakServer = server;
    Utils::Exit::registerStopCallback(
        [weakServer]()
        {
            if (auto s = weakServer.lock())
            {
                s->stop();
            }
        });

    // 启动：内部会打开监听器、启动巡检线程并开始 accept；失败会抛异常
    try
    {
        server->start();
    }
    catch (const std::exception& e)
    {
        Utils::Out::outErr(std::string("HTTP 服务器启动失败：") + e.what());
        return -1;
    }

    // IO 线程
    std::thread ioThread(
        [&io]()
        {
            try
            {
                io.run();
            }
            catch (const std::exception& e)
            {
                Utils::Out::outMsg(std::string("网络线程出现错误：") + e.what());
            }
            catch (...)
            {
                Utils::Out::outMsg("网络线程出现未知错误");
            }
        });

    Utils::Out::outMsg("HTTP 服务已启动，监听端口 " + std::to_string(HttpPort));
    Utils::Out::outMsg("试试：curl http://127.0.0.1:27080/        （同步回包）");
    Utils::Out::outMsg("     curl http://127.0.0.1:27080/slow   （异步回包，约 500ms）");

    // 主线程阻塞等退出信号（Ctrl+C / 关窗）
    Utils::Exit::waitExit();

    Utils::Out::outMsg("HTTP 服务器正在退出...");

    server->stop();
    ioThread.join();

    // 确保 Server 先于 io_context 析构（io 是 main 的局部变量，销毁顺序在其后）
    server.reset();

    Utils::Out::outMsg("HTTP 服务器已退出");
    return 0;
}
