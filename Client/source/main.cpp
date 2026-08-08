// Client/source/main.cpp
#include "NetClient.h"
#include "Utils.h"
#include "Message.h"

#include <boost/asio.hpp>

#include <memory>
#include <thread>
#include <iostream>
#include <csignal>

// 客户端启动函数
void RunClient(std::string host, std::string port, int ServiceID_)
{
    // 创建上下文
    boost::asio::io_context io;

    // 创建连接对象
    auto client = std::make_shared<Net::Client::Client>(io, ServiceID_);

    // 注册信号处理（Ctrl+C / 终止信号）
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait(
        [client, ServiceID_](const boost::system::error_code& ec, int signal_number)
        {
            if (!ec)
            {
                Utils::Out_Msg("收到退出信号(" + std::to_string(signal_number) + ")，正在优雅关闭...", ServiceID_);
                client->Stop();
            }
        });

    // 创建连接
    client->Connect(host, port);

    // 单独一个线程运行 io_context
    std::thread io_thread(
        [&io, client, ServiceID_]()
        {
            Utils::Out_Msg("客户端启动完毕", ServiceID_);
            io.run();
            // io_context 停止（连接失败或已关闭），通知主线程退出
            client->Stop();
        });

    // 主线程循环
    while (true)
    {
        auto [msg_id, msg] = client->WaitForMessage();

        // 收到终止信号
        if (msg_id == -1ULL && msg == "close")
        {
            Utils::Out_Msg("收到关闭通知，客户端即将退出", ServiceID_);
            break;
        }

        // 收到解析失败通知
        if (msg_id == -1ULL && msg == "resolve_failed")
        {
            Utils::Out_Err("解析地址失败，客户端即将退出", ServiceID_);
            break;
        }

        Utils::Out_Net_Msg(msg_id, "收到消息: " + msg, ServiceID_);

        Utils::Out_Msg("输入发送数据", 1);
        std::string str;
        std::cin >> str;
        client->ToSend(str);
    }

    // 等待接收处理完
    io_thread.join();
    Utils::Out_Msg("客户端已退出", ServiceID_);
}

int main()
{
    Utils::init();

    constexpr int ServiceID_ = 2;
    Utils::Out_Msg("客户端启动.....", ServiceID_);

    RunClient("127.0.0.1", "60000", ServiceID_);

    return 0;
}