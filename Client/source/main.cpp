/// @file        main.cpp
/// @brief       客户端入口（建立连接、发送消息、优雅退出）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// Client/source/main.cpp
#include "NetClient.h"
#include "Utils.h"
#include "Message.h"

#include <boost/asio.hpp>

#include <memory>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <iostream>
#include <limits>

/// @brief 存储客户端及其运行环境
/// @note 成员声明顺序决定析构逆序：iot -> hostport -> client -> io
///       必须保证 client 先于 io_context 析构，避免 socket 访问已释放的 io_context
struct ClientPtr
{
    /// @brief 保存会话的 io_context
    /// @warning 生命周期必须长于 client
    std::unique_ptr<boost::asio::io_context> io;

    /// @brief 客户端指针
    /// @note client客户端指针，hostport存储ip端口
    std::shared_ptr<Net::Client::Client> client;

    /// @brief 记录该客户端对应的ip与端口
    Net::Client::HostPort hostport;

    /// @brief io线程
    std::thread iot;
};

/// @brief 存储客户端数组
std::vector<ClientPtr> clients;

/// @brief 消息ID生成器
/// @details 连接测试固定用 0，业务消息从 1 开始自增，保证每条消息 ID 唯一
/// @note
std::atomic<unsigned long long> g_msg_id{0};

/// @brief 对应业务的回调
/// @param msg_id 消息id
/// @param msg 消息
void HandleWork(unsigned long long msg_id, std::string msg)
{
    Utils::Out::outNetMsg(msg_id, "服务器发来的消息：" + msg);
}

/// @brief      创建连接
/// @details    创建专属 io_context、客户端与线程，注册回调并异步发起连接
/// @param[in] HP 服务器地址与端口
/// @warning    须在 IO 线程启动前、由主线程调用，禁止多线程并发调用
/// @note
void CreateConnection(const Net::Client::HostPort& HP)
{
    Utils::Out::outMsg("正在连接 " + HP.host + ":" + HP.port);

    ClientPtr cp;
    cp.hostport = HP;

    // 先创建专属 io_context（生命周期必须长于 client，client 内部持有其引用）
    cp.io = std::make_unique<boost::asio::io_context>();

    // 创建线程独立的客户端：构造函数内部会异步发起连接
    // （此时 io_context 尚未 run，异步操作先入队，随后由 IO 线程驱动）
    cp.client =
        std::make_shared<Net::Client::Client>(*cp.io, std::make_unique<Net::Client::HandleFunction>(HandleWork), HP);

    // 先放入数组再启动线程，避免线程拿到被移动/已销毁的对象
    clients.push_back(std::move(cp));

    // 每连接 1 个线程驱动自己的 io_context
    // 注意：必须用 lambda 捕获；不能写 std::thread(io->run())
    //       —— 那会在当前线程直接阻塞执行 run()，并把返回值交给线程
    boost::asio::io_context* io = clients.back().io.get();
    clients.back().iot = std::thread([io]() { io->run(); });
}

// ============ main ============

int main()
{
    // 初始化控制台、日志目录、退出事件与信号处理（含 Ctrl+C / 关窗 / 系统关机）
    Utils::init();

    // 让日志打印出正确的服务名（Utils::Out 内部使用 serviceID）
    Utils::serviceID = ServiceID_RPCGateway;

    // 注册优雅退出回调：收到退出信号时停止所有连接
    // 必须在创建连接前注册，且回调内遍历数组，故对后加入的连接同样生效
    Utils::Exit::registerStopCallback(
        []()
        {
            Utils::Out::outMsg("正在关闭所有连接...");
            for (auto& cp : clients)
            {
                if (cp.client)
                {
                    cp.client->Stop();
                }
            }
        });

    Utils::Out::outMsg("请输入服务器地址与端口（例如：127.0.0.1 26990）");
    Net::Client::HostPort HP;
    if (!(std::cin >> HP.host >> HP.port))
    {
        Utils::Out::outErr("输入无效，客户端退出");
        return -1;
    }
    // 丢弃读取 host/port 后残留的换行符，供后面 getline 使用
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    // 建立连接（内部异步发起 Connect，必须在 IO 线程启动前完成）
    CreateConnection(HP);

    Utils::Out::outMsg("客户端运行中，输入消息回车发送（输入 quit 或按 Ctrl+C 退出）");

    // 输入线程：阻塞读控制台，读到一行就发给服务器
    // 说明：本线程可能长期阻塞在 getline 上（Ctrl+C 无法唤醒它），
    //       故退出时不 join，直接 detach，由进程退出统一回收
    std::thread input_thread(
        []()
        {
            std::string line;
            while (Utils::Exit::running.load() && std::getline(std::cin, line))
            {
                if (line.empty())
                {
                    continue;
                }

                // 主动退出
                if (line == "quit" || line == "exit")
                {
                    break;
                }

                if (clients.empty() || !clients.front().client)
                {
                    Utils::Out::outErr("连接尚未建立，消息未发送");
                    continue;
                }

                // 自增消息ID，保证同一条消息的ID唯一
                unsigned long long msg_id = ++g_msg_id;

                try
                {
                    clients.front().client->toSend(msg_id, line);
                    Utils::Out::outNetMsg(msg_id, "已发送：" + line);
                }
                catch (const std::exception& e)
                {
                    // toSend 对空消息/超长消息会抛异常，这里兜底防止线程退出
                    Utils::Out::outErr(std::string("发送失败：") + e.what());
                }
            }

            // 控制台输入结束（EOF 或 quit）也触发统一退出
            Utils::Exit::recviceExit();
        });
    input_thread.detach();

    // 主线程阻塞等待退出信号（由 Utils::Exit 统一处理 Ctrl+C / 关窗 / 全部连接关闭）
    Utils::Exit::waitExit();

    Utils::Out::outMsg("收到退出信号，正在关闭所有连接...");

    // 停止所有客户端的收发（幂等，可安全重复调用）
    for (auto& cp : clients)
    {
        if (cp.client)
        {
            cp.client->Stop();
        }
    }

    // 停止 io_context，让 run() 尽快返回
    for (auto& cp : clients)
    {
        if (cp.io)
        {
            cp.io->stop();
        }
    }

    // 回收 IO 线程
    for (auto& cp : clients)
    {
        if (cp.iot.joinable())
        {
            cp.iot.join();
        }
    }

    // 释放连接（成员析构逆序保证 client 先于 io_context 销毁）
    clients.clear();

    Utils::Out::outMsg("客户端已退出");
    return 0;
}
