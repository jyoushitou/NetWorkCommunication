/// @file        Utils.h
/// @brief       I/O、时间、优雅退出
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// 防止重复包含
#pragma once

#include "Message.h"

#ifdef _WIN32

#include <windows.h>

#endif

namespace Utils
{
    /// @brief      当前服务器ID
    /// @details    存储本服务进程的全局唯一ID
    /// @note
    extern int serviceID;

    /// @namespace  Time
    /// @brief      时间工具子模块
    /// @details    提供当前时间与日期的格式化获取
    /// @note
    namespace Time
    {
        /// @brief 获得当前时间
        /// @details 获得当前时间
        unsigned long long NowTime();

        /// @brief 计算时间差
        /// @details 计算时间戳到当前的时间差
        /// @param[in] oldtime 之前的时间戳
        /// @return 返回计算出来的时间
        unsigned long long Time(const time_t& oldtime);

        /// @brief      获取当前时间
        /// @details    返回当前时刻的格式化字符串
        /// @return     格式化后的时间字符串
        /// @note
        std::string NowTime_str();

        /// @brief      获取当前日期
        /// @details    返回当前日期的格式化字符串
        /// @return     格式化后的日期字符串
        /// @note
        std::string NowDay();

    } // namespace Time

    /// @brief      初始化
    /// @details    初始化控制台，并注册退出相关的回调
    /// @warning    应在程序启动早期调用
    /// @note
    void init();

    /// @namespace  Exit
    /// @brief      退出子模块
    /// @details    统一管理程序的优雅退出流程
    /// @note
    namespace Exit
    {

        /// @brief      退出标志
        /// @details    标识是否已触发退出
        /// @note
        inline std::atomic<bool> exit_flag(false);
        /// @brief      防止多次调用
        /// @details    保证退出流程仅执行一次
        /// @note
        inline std::atomic<bool> exit_called(false);
        /// @brief      运行标志
        /// @details    标识服务当前是否正在运行
        /// @note
        inline std::atomic<bool> running(true);

#ifdef _WIN32
        /// @brief      退出事件
        /// @details    统一退出事件：主线程 WaitExit() 阻塞等待
        /// @note
        inline HANDLE exit_event = nullptr;

        /// @brief      Windows 控制台关闭事件处理
        /// @details    响应控制台关闭等系统事件
        /// @param[in] ctrlType 控制台事件类型
        /// @return    处理成功返回 TRUE
        /// @note
        BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType);
#else
        /// @brief      退出互斥锁
        /// @details    保护退出条件变量
        /// @note
        inline std::mutex exit_mutex;
        /// @brief      退出条件变量
        /// @details    用于唤醒等待退出的线程
        /// @note
        inline std::condition_variable exit_cv;
        /// @brief      退出信号标志
        /// @details    标识是否已发出退出信号
        /// @note
        inline bool exit_signaled = false;
#endif

        /// @brief      注册停止回调
        /// @details    注册退出时需要回调的停止服务函数
        /// @param[in] cb 停止服务的回调函数
        /// @note
        void RegisterStopCallback(std::function<void()> cb);

        /// @brief      统一退出函数
        /// @details    执行优雅退出的完整流程
        /// @warning    应保证在退出过程中不被重复调用
        /// @note
        void GracefulShutdown();

        /// @brief      信号处理函数
        /// @details    按键/信号触发时的处理逻辑
        /// @param[in] sig 信号编号
        /// @note
        void Onsignal(int sig);

        /// @brief      清理资源
        /// @details    释放退出过程中占用的资源
        /// @note
        void CleanUp();

        /// @brief      阻塞等待退出信号
        /// @details    阻塞当前线程直至收到退出信号
        /// @note
        void WaitExit();

        /// @brief      主动触发退出
        /// @details    由外部主动发起的退出请求
        /// @note
        void RecviceExit();
    } // namespace Exit

    /// @namespace  File
    /// @brief      文件子模块
    /// @details    负责日志与普通文件的读写
    /// @note
    namespace File
    {
        /// @brief      日志的写入路径
        /// @details    日志文件存放的目录
        /// @note
        inline std::string logsdir = "logs";

        /// @brief      设置自定义的日志文件目录
        /// @details    修改日志文件存放目录
        /// @param[in] dir 日志目录路径
        /// @warning    须保证目录已存在或可创建
        /// @note
        void SetLogsDir(const std::string dir);

        /// @brief      检查日志目录
        /// @details    检查是否有logs文件夹，没有则创建
        /// @return     目录可用返回 true，否则返回 false
        /// @note
        bool CheckLogsDir();

        /// @brief      追加写入文件
        /// @details    以追加方式向指定文件写入内容
        /// @param[in] addr 文件路径
        /// @param[in] msg  待写入内容
        /// @return     写入成功返回 true，否则返回 false
        /// @note
        bool Out_File_add(const std::string addr, const std::string msg);
        /// @brief      覆写文件
        /// @details    以覆盖方式向指定文件写入内容
        /// @param[in] addr 文件路径
        /// @param[in] msg  待写入内容
        /// @return     写入成功返回 true，否则返回 false
        /// @note
        bool Out_File_wirte(const std::string addr, const std::string msg);

        /// @brief      写入日志
        /// @details    将消息写入日志文件
        /// @param[in] msg 日志内容
        /// @note
        void Out_Log(const std::string msg);
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
        void Out_Msg(const std::string msg);
        /// @brief      输出错误信息
        /// @details    错误信息输出
        /// @param[in] msg 输出内容
        /// @note
        void Out_Err(const std::string msg);

        /// @brief      网络输出
        /// @details    网络部分输出
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息内容
        /// @warning
        /// @note
        void Out_Net_Msg(unsigned long long msg_id, std::string msg);
    } // namespace Out

    /// @namespace  String
    /// @brief      字符串子模块
    /// @details    提供字符串分词等相关工具
    /// @note
    namespace String
    {
        /// @brief      数据分词
        /// @details    按指定分隔符切分字符串，默认以 | 为区分
        /// @param[in] str  待切分的字符串
        /// @param[in] post 起始位置
        /// @param[in] c    分隔字符
        /// @return     切分后的字符串集合
        /// @warning    分隔符参数默认值存在书写问题，使用前请确认
        /// @note
        std::vector<std::string> split(const std::string& str = "", const int& post = 0, const char& c = '|s');
    } // namespace String

} // namespace Utils