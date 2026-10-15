/// @file        main.cpp
/// @brief       测试服务器入口（启动 TCP 服务、处理退出信号）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// examples/Server/main.cpp
#include "NetServer.h"
#include "Utils.h"
#include "Message.h"

#include <boost/asio.hpp>

#include <memory>
#include <thread>
#include <string>

/// @brief      服务ID的存放位置说明
/// @details    serviceID 由 vcpkg 的 utils 包（Utils.dll）定义并导出：
///             Utils.h 里声明为 `extern Utils_API std::atomic<ServiceID> serviceID;`，
///             使用方只要在运行时赋值即可 —— 见 main() 里的 Utils::serviceID = RPCGateway。
/// @warning    dllimport 的导出符号禁止在消费侧再给定义：写了会直接编译报错（C2491），
///             即便绕过也会与 Utils.dll 内的定义冲突（LNK2005）。
///             只有回到「源码内置 / 静态库」形态时才需要在程序里补这份定义。

// 服务器监听端口
constexpr int ListenPort = 26990;

int main()
{
    // 设置当前服务ID（决定日志中的服务名）
    Utils::serviceID = RPCGateway;

    // 初始化控制台、日志目录、退出事件与信号处理（含 Ctrl+C / 关闭窗口）
    Utils::init();

    Utils::Out::outMsg("正在启动通讯服务...");

    // 创建 io_context
    boost::asio::io_context io;

    // 创建监听端点
    boost::asio::ip::tcp::endpoint ep(boost::asio::ip::tcp::v4(), ListenPort);

    // 业务回调：在 IO 线程中执行，无返回值；回复需在回调内调用 session->reply()
    auto handler = std::make_shared<Net::Server::HandleFunction>(
        [](const std::shared_ptr<Net::Server::Session>& session, const unsigned long long& msg_id,
           const std::string& msg)
        {
            Utils::Out::outMsg("收到客户端消息: " + msg);

            // TODO: 在这里编写你的业务处理逻辑

            session->reply(msg_id, "服务器已经收到消息");
        });

    // 创建服务器对象（会话空闲超时 60 秒、监控线程巡检周期 60 秒、每个会话发送队列上限 100MB）
    auto server = std::make_shared<Net::Server::Server>(io, ep, handler, 60, 60, 1024 * 1024 * 100);

    // 注册优雅退出回调：收到退出信号时停止服务器
    Utils::Exit::registerStopCallback([server]() { server->Stop(); });

    // 启动服务器（内部启动失效会话监控线程，并开始 accept）
    server->start();

    Utils::Out::outMsg("服务器启动，监听端口 " + std::to_string(ListenPort) + " ...等待连接中");

    // io_context 在独立线程中运行
    std::thread io_thread(
        [&io]()
        {
            try
            {
                io.run();
            }
            catch (const std::exception& e)
            {
                Utils::Out::outMsg("网络线程出现错误" + std::string(e.what()));
            }
            catch (...)
            {
                Utils::Out::outMsg("出现未知错误");
            }
        });

    // 主线程阻塞等待退出信号
    Utils::Exit::waitExit();

    Utils::Out::outMsg("服务器正在退出...");

    // 停止服务器（幂等，可安全重复调用）
    server->Stop();

    // 等待 IO 线程结束
    io_thread.join();

    Utils::Out::outMsg("服务器退出");

    return 0;
}