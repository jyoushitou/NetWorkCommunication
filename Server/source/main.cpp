/*
 * @file        main.cpp
 * @brief       服务器入口（启动 TCP 服务、处理退出信号）
 * @author      jyoushitou
 * @date        2026-09-16
 * @copyright   Copyright (c) 2026
 */

// Server/source/main.cpp
#include "NetServer.h"
#include "Utils.h"
#include "Message.h"

#include <boost/asio.hpp>

#include <memory>
#include <thread>
#include <string>

// 服务器监听端口
constexpr int kListenPort = 26990;

int main()
{
    // 设置当前服务ID（决定日志中的服务名）
    Utils::serviceID = ServiceID_RPCGateway;

    // 初始化控制台、日志目录、退出事件与信号处理（含 Ctrl+C / 关闭窗口）
    Utils::init();

    Utils::Out::outMsg("正在启动通讯服务...");

    // 创建 io_context
    boost::asio::io_context io;

    // 创建监听端点
    boost::asio::ip::tcp::endpoint ep(boost::asio::ip::tcp::v4(), kListenPort);

    // 业务回调：在 IO 线程中执行，返回值作为回复内容发回客户端
    auto handler = std::make_shared<Net::Server::HandleFunction>(
        [](const std::shared_ptr<Net::Server::Session>&, const std::string& msg) -> std::string
        {
            Utils::Out::outMsg("收到客户端消息: " + msg);

            // TODO: 在这里编写你的业务处理逻辑

            // 返回给客户端的响应
            return "服务器已收到！";
        });

    // 创建服务器对象
    auto server = std::make_shared<Net::Server::Server>(io, ep, handler, 5);

    // 注册优雅退出回调：收到退出信号时停止服务器
    Utils::Exit::registerStopCallback([server]() { server->Stop(); });

    // 开始接收连接
    server->StartAccept();

    Utils::Out::outMsg("服务器启动，监听端口 " + std::to_string(kListenPort) + " ...等待连接中");

    // io_context 在独立线程中运行
    std::thread io_thread([&io]() { io.run(); });

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