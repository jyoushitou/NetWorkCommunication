/// @file        Utils.cpp
/// @brief       I/O、时间、优雅退出
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// 头文件
#include "Utils.h"

#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>

#include <ctime>
#include <thread>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <direct.h>
#else
#include <csignal>
#include <sys/types.h>
#endif

namespace Utils
{
    /// @brief      初始化
    /// @details    初始化控制台，并注册退出相关的回调
    /// @warning    应在程序启动早期调用
    /// @note
    void init()
    {
#ifdef _WIN32

        // 检验退出事件是否已创建，未创建时手动重置
        if (!Exit::exit_event)
            // 手动重置
            Exit::exit_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        // 设置控制台的编码格式
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCtrlHandler(Exit::ConsoleCtrlHandler, TRUE);
#else
        // Linux/macOS：注册信号处理器，Ctrl+C 或 kill 时触发优雅退出
        std::signal(SIGINT, Exit::onsignal);
        std::signal(SIGTERM, Exit::onsignal);
        // 可选：忽略 SIGPIPE 防止写入已关闭 socket 导致进程崩溃
        std::signal(SIGPIPE, SIG_IGN);
#endif
    }

    /// @namespace  Time
    /// @brief      时间工具子模块
    /// @details    提供当前时间与日期的格式化获取
    /// @note
    namespace Time
    {
        /// @brief      获取本地时间戳
        /// @details    将时间戳转换为本地 std::tm 结构
        /// @param[out] local 输出的本地时间结构
        /// @param[in]  now   待转换的时间戳
        /// @note       平台相关，Windows 使用 localtime_s，Linux/macOS 使用localtime_r
        static void PushTimeToLocal(std::tm& local, time_t now)
        {
#if _WIN32
            // 按照Windows编码的获取
            localtime_s(&local, &now);
#else
            // 按照Linux编码的获取
            localtime_r(&now, &local);
#endif
        }

        /// @brief 获得Local本地时间戳
        /// @details 创建后获得已经赋值过后的Local,now
        static std::pair<std::tm, time_t> GetLocalNow()
        {
            // 现在的时间的时间戳获得
            time_t now = std::time(nullptr);
            std::tm local{};

            // 获得local和now
            PushTimeToLocal(local, now);

            return std::pair<std::tm, time_t>{local, now};
        }

        /// @brief 获得当前时间
        /// @details 获得当前时间
        time_t nowTime()
        {
            return GetLocalNow().second;
        }

        /// @brief 获得Local本地时间戳
        /// @details 创建后获得已经赋值过后的Local
        static std::tm GetLocal()
        {
            return GetLocalNow().first;
        }

        /// @brief 计算时间差
        /// @details 计算时间戳到当前的时间差
        /// @param[in] oldtime 之前的时间戳
        /// @return 返回计算出来的时间
        time_t computeTime(const time_t& oldtime)
        {
            if (time < 0)
            {
                Utils::Out::outMsg("传入时间错误");
            }

            // 获得当前的时间
            time_t now = nowTime();

            return now - oldtime;
        }

        /// @brief      获取当前时间
        /// @details    返回当前时刻的格式化字符串
        /// @return     格式化后的时间字符串
        /// @note
        std::string getNowtime()
        {
            std::tm local = GetLocal();
            std::ostringstream oss;
            oss << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
            return oss.str();
        }

        /// @brief      获取当前日期
        /// @details    返回当前日期的格式化字符串（形如 yyyy-m-d-logs）
        /// @return     格式化后的日期字符串
        /// @note
        std::string getNowDay()
        {
            // 现在的时间的时间戳
            std::tm local = GetLocal();

            // tm_year 从 1900 年开始算
            int year = local.tm_year + 1900;
            // tm_mon 范围是 0~11
            int month = local.tm_mon + 1;
            // 1~31
            int day = local.tm_mday;

            return std::to_string(year) + "-" + std::to_string(month) + "-" + std::to_string(day) + "-logs";
        }

    } // namespace Time

    /// @namespace  Exit
    /// @brief      退出子模块
    /// @details    统一管理程序的优雅退出流程
    /// @note
    namespace Exit
    {
        /// @brief      停止回调列表
        /// @details    保存所有已注册的停止回调
        /// @note
        inline std::vector<std::function<void()>> stop_callbacks;
        /// @brief      回调互斥锁
        /// @details    保护停止回调列表的互斥锁
        /// @note
        inline std::mutex callbacks_mutex;

        /// @brief      注册停止回调
        /// @details    注册退出时需要回调的停止服务函数
        /// @param[in] cb 停止服务的回调函数
        /// @note
        void registerStopCallback(std::function<void()> cb)
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex);
            stop_callbacks.push_back(std::move(cb));
        }

        /// @brief      统一退出函数
        /// @details    执行优雅退出的完整流程
        /// @warning    应保证在退出过程中不被重复调用
        /// @note
        void gracefulShutdown()
        {
            bool expected = false;

            if (exit_called.compare_exchange_strong(expected, true))
            {
                Out::outMsg("收到退出信号，正在停止服务器...");

                exit_flag = true;

                running = false;

                // 遍历所有已注册的停止回调（每个服务只注册一个汇总回调）
                std::vector<std::function<void()>> callbacks;
                {
                    std::lock_guard<std::mutex> lock(callbacks_mutex);
                    callbacks = stop_callbacks;
                }
                for (auto& cb : callbacks)
                {
                    if (cb)
                        cb();
                }
            }

#ifdef _WIN32
            if (exit_event)
            {
                SetEvent(exit_event);
            }
#else
            {
                // Linux/macOS：通知 waitExit() 返回
                std::lock_guard<std::mutex> lock(exit_mutex);
                exit_signaled = true;
            }
            exit_cv.notify_all();
#endif
        }

        /// @brief      阻塞等待退出信号
        /// @details    阻塞当前线程直至收到退出信号
        /// @note
        void waitExit()
        {
#ifdef _WIN32
            if (!exit_event)
                exit_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            WaitForSingleObject(exit_event, INFINITE);
#else
            while (running.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
        }

        /// @brief      主动触发退出
        /// @details    由外部主动发起的退出请求
        /// @note
        void recviceExit()
        {
            gracefulShutdown();
        }

        /// @brief      信号处理函数
        /// @details    按键/信号触发时的处理逻辑
        /// @param[in] sig 信号编号（当前未使用）
        /// @note
        void onsignal(int sig)
        {
            gracefulShutdown();
        }

#ifdef _WIN32
        /// @brief      Windows 控制台关闭事件处理
        /// @details    响应控制台关闭等系统事件
        /// @param[in] ctrlType 控制台事件类型
        /// @return    处理成功返回 TRUE
        /// @note
        BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
        {
            switch (ctrlType)
            {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                running = false;
                gracefulShutdown();
                return TRUE;
            default:
                return FALSE;
            }
        }
#endif
    } // namespace Exit

    /// @namespace  File
    /// @brief      文件子模块
    /// @details    负责日志与普通文件的读写
    /// @note
    namespace File
    {
        /// @brief      设置自定义的日志文件目录
        /// @details    修改日志文件存放目录
        /// @param[in] dir 日志目录路径
        /// @warning    须保证目录已存在或可创建
        /// @note
        void setLogsDir(const std::string dir)
        {
            logsdir = dir;
        }

        /// @brief      检查日志目录
        /// @details    检查是否有logs文件夹，没有则创建
        /// @return     目录可用返回 true，否则返回 false
        /// @note
        bool checkLogsDir()
        {
#ifdef _WIN32
            if (_mkdir(logsdir.c_str()) == 0)
                return true;
            return errno == EEXIST;
#else
            if (mkdir(logsdir.c_str(), 0755) == 0)
                return true;
            return errno == EEXIST;
#endif
        }

        /// @brief      追加写入文件
        /// @details    以追加方式向指定文件写入内容
        /// @param[in] addr 文件路径
        /// @param[in] msg  待写入内容
        /// @return     写入成功返回 true，否则返回 false
        /// @note
        bool outFileAdd(const std::string addr, const std::string msg)
        {
            // 默认打开模式是覆盖写
            std::ofstream out(addr.c_str(), std::ios::app);
            if (!out)
            {
                std::cerr << "打开文件失败" << std::endl;
                return 1;
            }
            out << msg << std::endl;
            out.close();
            return 0;
        }

        /// @brief      写入日志
        /// @details    将消息写入日志文件
        /// @param[in] msg 日志内容
        /// @note
        void outLog(const std::string msg)
        {
            // 确认是否有这个文件夹
            if (!checkLogsDir())
            {
                std::cerr << "创建logs失败" << std::endl;
            }
            std::string addr = logsdir + "/" + Time::getNowDay() + ".txt";
            if (File::outFileAdd(addr, msg))
                std::cerr << "写入日志失败" << std::endl;
        }
    } // namespace File

    /// @namespace  Out
    /// @brief      输出子模块
    /// @details    统一控制台与网络的输出接口
    /// @note
    namespace Out
    {

        /// @brief      输出信息
        /// @details    普通信息输出
        /// @param[in] msg 输出内容
        /// @note
        void outMsg(const std::string msg)
        {
            std::string Out_Str = "[" + ServiceID[serviceID] + "][INFO]" + Time::getNowtime() + " " + msg;
            std::cout << Out_Str << std::endl;
            File::outLog(Out_Str);
        }

        /// @brief      输出错误信息
        /// @details    错误信息输出
        /// @param[in] msg 输出内容
        /// @note
        void outErr(const std::string msg)
        {
            std::string Out_Str = "[" + ServiceID[serviceID] + "][ERROR]" + Time::getNowtime() + " " + msg;
            std::cerr << Out_Str << std::endl;
            File::outLog(Out_Str);
        }

        /// @brief      网络输出
        /// @details    网络部分输出
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息内容
        /// @warning
        /// @note
        void outNetMsg(unsigned long long msg_id, std::string msg)
        {
            outMsg("[信息ID:" + std::to_string(msg_id) + "]" + msg);
        }
    } // namespace Out

    /// @namespace  String
    /// @brief      字符串子模块
    /// @details    提供字符串分词等相关工具
    /// @note
    namespace String
    {
        /// @brief      数据分词
        /// @details    按指定分隔符切分字符串
        /// @param[in] str  待切分的字符串
        /// @param[in] post 起始位置
        /// @param[in] c    分隔字符
        /// @return     切分后的字符串集合
        /// @warning    当前函数体为空，尚未实现
        /// @note
        std::vector<std::string> split(const std::string& str, const int& post, const char& c)
        {
        }
    } // namespace String
} // namespace Utils
