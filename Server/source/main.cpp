// Server/source/main.cpp
#include "NetServer.h"
#include "Utils.h"
#include "Message.h"

#include <boost/asio.hpp>

#include <memory>
#include <thread>
#include <iostream>
#include <csignal>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

// 全局服务器指针，供信号处理函数使用
std::shared_ptr<Net::Server::Server> g_server;

// 退出标志
std::atomic<bool> g_exit_flag{false};

// 防止 Stop() 被多次调用的标志
std::atomic<bool> g_stop_called{false};

// 统一优雅退出逻辑（保证只执行一次）
void GracefulShutdown()
{
    bool expected = false;
    if (g_stop_called.compare_exchange_strong(expected, true))
    {
        Utils::Out_Msg("收到退出信号，正在停止服务器...", 1);
        g_exit_flag = true;
        if (g_server)
        {
            g_server->Stop();
        }
    }
}

// Ctrl+C / SIGTERM 处理函数
void OnSignal(int)
{
    GracefulShutdown();
}

#ifdef _WIN32
// Windows 控制台关闭事件处理（taskkill、关闭窗口等）
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    switch (ctrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        GracefulShutdown();
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

// 服务器启动函数
void RunServer(int port, int ServiceID_)
{
    Utils::Out_Msg("正在启动通讯端口", ServiceID_);

    // 创建上下文
    boost::asio::io_context io;

    // 创建监听端点
    boost::asio::ip::tcp::endpoint ep(boost::asio::ip::tcp::v4(), port);

    // 创建服务器对象
    g_server = std::make_shared<Net::Server::Server>(io, ep, ServiceID_);

    // 开始接收连接
    g_server->StartAccept();

    Utils::Out_Msg("服务器启动，监听端口 " + std::to_string(port) + " ...等待连接中", ServiceID_);

    // 注册 Ctrl+C 处理
    std::signal(SIGINT, OnSignal);

#ifdef _WIN32
    // 注册 Windows 控制台事件处理（taskkill / 关闭窗口等也能优雅退出）
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

    // 单独一个线程运行 io_context
    std::thread io_thread([&io]() { io.run(); });

    // 主线程循环：等待消息并处理，然后回复客户端
    while (true)
    {
        auto [session, msg_id, msg] = g_server->WaitForMessage();

        // 收到终止信号
        if (!session && msg_id == -1ULL && msg == "close")
        {
            Utils::Out_Msg("服务器正在退出...", ServiceID_);
            break;
        }

        Utils::Out_Msg("收到客户端消息[id=" + std::to_string(msg_id) + "]: " + msg, ServiceID_);

        // TODO: 在这里编写你的业务处理逻辑
        // 处理完消息后，通过 session->Reply() 回复给客户端

        // 示例：回显给客户端
        session->Reply(msg_id, "服务器已收到！");
    }

    // 停止服务器（幂等，可安全重复调用）
    g_server->Stop();

    // 等待接收处理完
    io_thread.join();
}

int main()
{
    Utils::init();

    RunServer(60000, 1);

    Utils::Out_Msg("服务器退出", 1);

    return 0;
}