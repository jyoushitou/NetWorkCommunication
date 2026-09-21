// Client/source/main.cpp
#include "NetClient.h"
#include "Utils.h"
#include "Message.h"

#include <boost/asio.hpp>

#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>

struct ConnItem
{
    std::unique_ptr<boost::asio::io_context> io;
    std::shared_ptr<Net::Client::Client> client;
    std::thread io_thread;
};

// ============ 全局状态 ============
/// @brief      连接数组
/// @details    储存所有连接，每条连接含独立 io_context、客户端与线程
/// @warning    创建连接完成后才可访问，禁止在多线程中同时增删
/// @note
std::vector<std::shared_ptr<ConnItem>> g_conns;
/// @brief      运行标志
/// @details    标记客户端是否运行中（回调线程只碰这个）
/// @note
std::atomic<bool> g_running{true};
/// @brief      总连接数
/// @details    记录创建的连接总数（CreateConnection 中递增）
/// @note
std::atomic<size_t> g_total_conns{0};
/// @brief      已关闭连接数
/// @details    记录已关闭的连接数（OnClose 中递增）
/// @note
std::atomic<size_t> g_closed_conns{0};

// ============ 业务回调（各自连接的 IO 线程中执行） ============

/// @brief      消息处理
/// @details    收到消息后在对应连接的 IO 线程中执行，打印日志
/// @param[in] idx 连接下标
/// @param[in] serviceID 服务ID，用于日志打印
/// @param[in] msg_id 消息全局唯一ID
/// @param[in] msg 消息体
/// @warning    仅在 IO 线程内被调用，禁止长时间阻塞
/// @note
void Work(size_t idx, int serviceID, unsigned long long msg_id, const std::string& msg)
{
    Utils::Out::outNetMsg(msg_id, "线程" + std::to_string(idx) + "收到消息: " + msg);
}

/// @brief      关闭回调
/// @details    连接彻底关闭后执行，统计已关闭数并在全部关闭后唤醒主线程
/// @param[in] idx 连接下标
/// @param[in] serviceID 服务ID，用于日志打印
/// @warning    仅在 IO 线程内被调用，禁止长时间阻塞
/// @note
void close(size_t idx, int serviceID)
{
    Utils::Out::outMsg("正在关闭:" + std::to_string(static_cast<int>(10 + idx)) + "线程");

    // 统计已关闭数：fetch_add 返回旧值，+1 得到新值
    size_t closed = g_closed_conns.fetch_add(1) + 1;
    size_t remain = g_total_conns.load() - closed;
    Utils::Out::outMsg("当前剩余线程数，" + std::to_string(remain));

    // 全部关闭后，走 Utils 统一退出流程唤醒主线程
    if (closed == g_total_conns.load())
    {
        Utils::Exit::recviceExit();
    }
}

// ============ 创建连接 ============

/// @brief      创建连接
/// @details    创建专属 io_context、客户端与线程，注册回调并异步发起连接
/// @param[in] idx 连接下标
/// @param[in] serviceID 服务ID，用于日志打印
/// @param[in] host 服务器地址
/// @param[in] port 服务器端口
/// @warning    须在 IO 线程启动前调用，禁止多线程并发调用
/// @note
void CreateConnection(size_t idx, int serviceID, const std::string& host, const std::string& port)
{
    Utils::Out::outMsg("正在连接");

    // 总连接数统计自增
    g_total_conns.fetch_add(1);

    // 创建连接项（含 io_context、客户端与线程）
    auto conn = std::make_shared<ConnItem>();

    // 创建专属io_context
    conn->io = std::make_unique<boost::asio::io_context>();

    // 创建线程独立的客户端
    conn->client = std::make_shared<Net::Client::Client>(*conn->io, serviceID);

    // 注册回调（捕获 idx，避免共享状态）
    conn->client->SetMessageCallback([idx, serviceID](unsigned long long id, std::string msg)
                                     { Work(idx, serviceID, id, msg); });

    // 设置关闭回调
    conn->client->SetCloseCallback([idx, serviceID]() { close(idx, serviceID); });

    // 异步连接，先发起连接保证 io_context 中有任务
    conn->client->Connect(host, port);

    // 每连接 1 个线程驱动自己的 io_context
    conn->io_thread = std::thread(
        [conn]
        {
            conn->io->run(); // 阻塞直到该连接 Stop() 后 io_context 无任务
        });

    // 将独立io_context加入数组
    g_conns.push_back(conn);
}

// ============ main ============

int main()
{
    // 初始化控制台、日志目录、退出事件与信号处理（含 Ctrl+C / 关窗 / 系统关机）
    Utils::init();

    int serviceID = ServiceID_RPCGateway;
    // 让日志打印出正确的服务名（Utils::Out 内部使用 serviceID）
    Utils::serviceID = serviceID;

    // 注册优雅退出回调：收到退出信号时关闭所有连接。
    // ⚠️ 本回调在系统信号线程内执行，只做线程安全的 Stop()，绝不做 join/阻塞等待
    Utils::Exit::registerStopCallback(
        []()
        {
            g_running = false;
            for (auto& conn : g_conns)
            {
                if (conn->client)
                    conn->client->Stop();
            }
        });

    Utils::Out::outMsg("输入网址");
    std::string ipv4 = "";
    std::cin >> ipv4;

    CreateConnection(1, serviceID, ipv4, "26990");

    Utils::Out::outMsg("客户端运行中，按 Ctrl+C 退出");

    std::thread input_thread(
        []
        {
            std::string str;
            while (g_running && std::cin >> str)
            {
                // 防御性检查：g_conns 在连接创建完成后才启动本线程，非空
                if (!g_conns.empty() && g_conns[0]->client)
                    g_conns[0]->client->toSend(0, str);
            }
        });

    // 阻塞等待退出信号（由 Utils::Exit 统一处理 Ctrl+C / 关窗 / 全部连接关闭）
    Utils::Exit::waitExit();

    input_thread.detach();

    // ===== 优雅关闭流程（回到主线程执行，安全） =====
    Utils::Out::outMsg("收到退出信号，正在关闭所有连接...");

    // Stop 已在退出回调中投递，这里只需等待所有 IO 线程结束
    // 流程：Stop -> close -> actuallyClose -> toClosed -> io 无任务 -> run() 返回
    for (auto& conn : g_conns)
        if (conn->io_thread.joinable())
            conn->io_thread.join();

    Utils::Out::outMsg("客户端已退出");
    return 0;
}