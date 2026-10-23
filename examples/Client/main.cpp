/// @file        main.cpp
/// @brief       测试客户端入口（控制台输入发送、多连接管理、优雅退出）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// examples/Client/main.cpp
#include "NetClient.h"
#include "Utils.h"
#include "Message.h"

#include <boost/asio.hpp>

#include <memory>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <iostream>
#include <limits>

/// @brief      服务ID的存放位置说明
/// @details    serviceID 由 vcpkg 的 utils 包（Utils.dll）定义并导出：
///             Utils.h 里声明为 `extern Utils_API std::atomic<ServiceID> serviceID;`，
///             使用方只要在运行时赋值即可 —— 见 main() 里的 Utils::serviceID = RPCGateway。
/// @warning    dllimport 的导出符号禁止在消费侧再给定义：写了会直接编译报错（C2491），
///             即便绕过也会与 Utils.dll 内的定义冲突（LNK2005）。
///             只有回到「源码内置 / 静态库」形态时才需要在程序里补这份定义。

/// @brief 存储客户端及其运行环境
/// @note 成员声明顺序决定析构逆序：先析构 iot（IO 线程），再析构 client；
///       对应的 io_context 由全局 g_ios 持有，必须保证它晚于 client 释放
struct ClientPtr
{
    /// @brief 客户端指针
    /// @note client 指向本连接对应的客户端对象（地址与端口保存在其内部）
    std::shared_ptr<Net::Client::Client> client;

    /// @brief io线程
    /// @details 驱动该客户端专属 io_context 的线程
    std::thread iot;
};

/// @brief 存储各连接专属的 io_context
/// @details ClientPtr 不含 io 成员（结构体不可改），故由本全局容器统一持有
/// @note 声明顺序即析构逆序：g_ios 先声明 → 后析构，保证 io_context 比 client 活得久
std::vector<std::unique_ptr<boost::asio::io_context>> g_ios;

/// @brief 保护 clients 的互斥量
/// @details 输入线程（取指针发送）、退出回调线程与主线程（停止/回收）都会访问 clients，必须加锁
/// @note
std::mutex g_clientsMutex;

/// @brief 存储客户端数组
std::vector<ClientPtr> clients;

/// @brief 消息ID生成器
/// @details 连接测试固定用 0，业务消息从 1 开始自增，保证每条消息 ID 唯一
/// @note
std::atomic<unsigned long long> g_msg_id{0};

/// @brief 本地运行标志
/// @details 新版 Utils 头文件没有对外暴露退出运行标志（旧版是 Utils::Exit::running），
///          这里自己维护一份：退出流程启动时会回调已注册的停止函数，借此置为 false
/// @note
std::atomic<bool> g_running{true};

/// @brief 对应业务的回调
/// @param[in] msg_id 消息id
/// @param[in] msg 消息
/// @note 在对应连接的 IO 线程内被调用，禁止长时间阻塞
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

    // 先创建专属 io_context（生命周期必须长于 client，client 内部持有其引用）
    // 由全局容器持有：vector 重分配只移动 unique_ptr，不移动 io_context 本体，
    // 因此引用保持有效
    g_ios.push_back(std::make_unique<boost::asio::io_context>());
    boost::asio::io_context& io = *g_ios.back();

    ClientPtr cp;

    // 创建本连接专属的客户端（构造函数只做参数保存，真正的异步连接由下面的 Connect() 发起）
    cp.client = std::make_shared<Net::Client::Client>(
        io, std::make_unique<Net::Client::HandleFunction>(HandleWork), HP,
        std::make_unique<Net::Client::CloseFunction>([](const boost::system::error_code) {}), 1024 * 1024 * 100);

    // 连接
    cp.client->Connect();

    // 先放入数组再启动线程，避免线程拿到被移动/已销毁的对象
    {
        // 加锁：此刻输入线程可能还没起来，但退出回调线程已经可能遍历 clients
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        clients.push_back(std::move(cp));

        // 每连接 1 个线程驱动自己的 io_context
        // 注意：必须用 lambda 捕获；不能写 std::thread(io.run())
        //       —— 那会在当前线程直接阻塞执行 run()，并把返回值交给线程
        clients.back().iot = std::thread([&io]() { io.run(); });
    }
}

// ============ main ============

int main()
{
    // 初始化控制台、日志目录、退出事件与信号处理（含 Ctrl+C / 关窗 / 系统关机）
    Utils::init();

    // 设置当前服务ID（决定日志中的服务名）
    Utils::serviceID = RPCGateway;

    // 注册优雅退出回调：收到退出信号时停止所有连接
    // 必须在创建连接前注册，且回调内遍历数组，故对后加入的连接同样生效
    Utils::Exit::registerStopCallback(
        []()
        {
            Utils::Out::outMsg("正在关闭所有连接...");
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            for (auto& cp : clients)
            {
                if (cp.client)
                {
                    cp.client->stop();
                }
            }
        });

    // 退出流程启动时同步本地运行标志，语义与旧版 Utils::Exit::running 一致
    Utils::Exit::registerStopCallback([]() { g_running = false; });

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
            while (g_running.load() && std::getline(std::cin, line))
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

                // 取出当前连接：加锁拷贝 shared_ptr，避免与主线程/退出回调的停止、回收竞争
                std::shared_ptr<Net::Client::Client> client;
                {
                    std::lock_guard<std::mutex> lock(g_clientsMutex);
                    if (!clients.empty())
                    {
                        client = clients.front().client;
                    }
                }

                if (!client)
                {
                    Utils::Out::outErr("连接尚未建立，消息未发送");
                    continue;
                }

                // 自增消息ID，保证同一条消息的ID唯一
                unsigned long long msg_id = ++g_msg_id;

                // toSend 不再抛异常：返回 false 表示消息非法（空 / 超过 MAX_LENGTH）；
                // 发送队列积压超限会在 IO 线程内丢弃并记日志
                if (client->toSend(msg_id, line))
                {
                    Utils::Out::outNetMsg(msg_id, "已发送：" + line);
                }
                else
                {
                    Utils::Out::outErr("消息为空或超过长度上限，未发送");
                }
            }

            // 控制台输入结束（EOF 或 quit）也触发统一退出
            Utils::Exit::recviceExit();
        });
    input_thread.detach();

    // 主线程阻塞等待退出信号（由 Utils::Exit 统一处理 Ctrl+C / 关窗 / 全部连接关闭）
    Utils::Exit::waitExit();

    Utils::Out::outMsg("收到退出信号，正在关闭所有连接...");

    // 请求各连接优雅关闭（stop 内部会 post 关闭请求），并回收各 IO 线程
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        for (auto& cp : clients)
        {
            if (cp.client)
            {
                cp.client->stop();
            }
        }

        // 不调用 io->stop()：等所有异步操作结束后 run() 会自然返回
        for (auto& cp : clients)
        {
            if (cp.iot.joinable())
            {
                cp.iot.join();
            }
        }
    }

    // 注意：这里**不要** clients.clear() / g_ios.clear()。
    // 输入线程是 detach 的，可能还阻塞在 getline 上；销毁容器会让它访问已释放的内存。
    // 进程即将结束，交给静态析构回收即可（g_ios 先声明 → 后析构，保证 io_context 比 client 活得久）。

    Utils::Out::outMsg("客户端已退出");
    return 0;
}