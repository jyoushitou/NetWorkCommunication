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

#ifdef _WIN32
#include <windows.h>
#endif

struct ConnItem
{
    std::unique_ptr<boost::asio::io_context> io;
    std::shared_ptr<Net::Client::Client> client;
    std::thread io_thread;
};

// ============ 全局状态 ============
// 储存18 条连接
std::vector<std::shared_ptr<ConnItem>> g_conns;
// 运行标志（回调线程只碰这个）
std::atomic<bool> g_running{true};
// 总连接数（CreateConnection 中递增）
std::atomic<size_t> g_total_conns{0};
// 已关闭连接数（OnClose 中递增）
std::atomic<size_t> g_closed_conns{0};
// 退出事件（主线程等待它）
HANDLE g_exit_event = nullptr;

// ============ Windows 控制台信号处理 ============

BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType)
{
    switch (dwCtrlType)
    {
    case CTRL_C_EVENT:        // Ctrl+C
    case CTRL_BREAK_EVENT:    // Ctrl+Break
    case CTRL_CLOSE_EVENT:    // 用户点关闭窗口
    case CTRL_SHUTDOWN_EVENT: // 系统关机
    {
        // 在 Windows 专用回调线程中执行：
        // ⚠️ 只允许做这两件事，绝不打印日志、绝不调 Stop()（会内部 post，有线程安全问题风险）
        g_running = false;
        SetEvent(g_exit_event); // 唤醒主线程
        return TRUE;            // 已处理，阻止进程被强杀
    }
    default:
        return FALSE; // 其他信号交给系统默认
    }
}

// ============ 业务回调（各自连接的 IO 线程中执行） ============

void Work(size_t idx, int serviceID, unsigned long long msg_id, const std::string& msg)
{
    Utils::Out_Net_Msg(msg_id, "线程" + std::to_string(idx) + "收到消息: " + msg, serviceID);
}

void Close(size_t idx, int serviceID)
{
    Utils::Out_Msg("正在关闭:" + std::to_string(static_cast<int>(10 + idx)) + "线程", serviceID);

    // 统计已关闭数（fetch_add 返回旧值，+1 得到新值）
    size_t closed = g_closed_conns.fetch_add(1) + 1;
    size_t remain = g_total_conns.load() - closed;
    Utils::Out_Msg("当前剩余线程数，" + std::to_string(remain), serviceID);

    // 全部关闭后唤醒主线程
    if (closed == g_total_conns.load())
    {
        g_running = false;
        SetEvent(g_exit_event);
    }
}

// ============ 创建连接 ============

void CreateConnection(size_t idx, int serviceID, const std::string& host, const std::string& port)
{
    Utils::Out_Msg("正在连接", serviceID);

    // 线程数统计自增
    g_total_conns.fetch_add(1);

    // 创建io_context的线程指针
    auto conn = std::make_shared<ConnItem>();

    // 创建专属io_context
    conn->io = std::make_unique<boost::asio::io_context>();

    // 创建线程独立的客户端
    conn->client = std::make_shared<Net::Client::Client>(*conn->io, serviceID);

    // 注册回调（捕获 idx，避免共享状态）
    conn->client->SetMessageCallback([idx, serviceID](unsigned long long id, std::string msg)
                                     { Work(idx, serviceID, id, msg); });

    // 设置关闭回调
    conn->client->SetCloseCallback([idx, serviceID]() { Close(idx, serviceID); });

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
    Utils::init();

    int serviceID = 1;

    // 1. 创建自动复位事件（初始无信号）
    g_exit_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_exit_event)
    {
        Utils::Out_Err("创建退出事件失败", 1);
        return 1;
    }

    // 2. 注册控制台信号处理（必须在创建连接之前）
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE))
    {
        Utils::Out_Err("注册控制台处理函数失败", 1);
        CloseHandle(g_exit_event);
        return 1;
    }

    // // 3. 创建 18 条内网连接（地址按实际填）
    // for (size_t i = 0; i < 18; ++i)
    // {
    //     CreateConnection(i, "127.0.0.1", "60000");
    // }

    CreateConnection(1, serviceID, "127.0.0.1", "60000");

    Utils::Out_Msg("客户端运行中，按 Ctrl+C 退出", 1);

    std::thread input_thread(
        []
        {
            std::string str;
            while (g_running && std::cin >> str)
            {
                // 防御性检查：g_conns 在连接创建完成后才启动本线程，非空
                if (!g_conns.empty())
                    g_conns[0]->client->ToSend(str);
            }
        });

    // 4. 主线程完全挂起，等待退出事件被 SetEvent
    //    ⚠️ 此时不占任何 CPU，这是事件对象比 sleep 轮询的绝对优势
    WaitForSingleObject(g_exit_event, INFINITE);

    input_thread.detach();

    // ===== 优雅关闭流程（现在回到主线程执行，安全） =====
    Utils::Out_Msg("收到退出信号，正在关闭所有连接...", 1);

    // 5. Stop 所有连接：内部 post 到各自 IO 线程，线程安全
    for (auto& conn : g_conns)
        conn->client->Stop();

    // 6. 等待所有 IO 线程结束
    //    流程：Stop -> Close -> ActuallyClose -> OnClosed -> io 无任务 -> run() 返回
    for (auto& conn : g_conns)
        if (conn->io_thread.joinable())
            conn->io_thread.join();

    // 7. 清理
    CloseHandle(g_exit_event);
    Utils::Out_Msg("客户端已退出", 1);
    return 0;
}