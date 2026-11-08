/// @file        NetMsgId.h
/// @brief       协议保留消息 ID（0–999）与业务 ID 的唯一定义处
/// @details     Net 的 12 字节头部里只有 msg_id，没有"命令"字段，所以**ID 本身就是命令**：
///              保留区间 [0, 999] 由库/框架占用，业务消息从 1000 起。
///              这个头文件是这段约定的单一来源：新增特殊 ID 只改这里，
///              服务端与客户端都从这里取名字，避免两端各写一份数字。
/// @author      jyoushitou
/// @date        2026-11-08
/// @copyright   Copyright (c) 2026
#pragma once

#include <array>
#include <string_view>

namespace Net
{
    /// @brief 业务消息的起始 ID
    /// @details 低于它的 ID 都属于保留区间，由库或框架定义语义
    inline constexpr unsigned long long kFirstBusinessMsgId = 1000ULL;

    /// @brief 保留区间上界（含）
    inline constexpr unsigned long long kReservedMsgIdMax = kFirstBusinessMsgId - 1ULL;

    /// @namespace   MsgId
    /// @brief       保留消息 ID 的命名与查询
    /// @details     这里只登记"库/框架自己会用"的 ID；业务 ID（>= 1000）不在此列
    /// @note        协议本身不校验区间（`toSend` 接受任意 uint64），区间是**约定**，
    ///              由服务端/客户端在业务层自行遵守
    namespace MsgId
    {
        /// @brief 连接测试：客户端连上后自动发的那条
        /// @details payload 固定为 startClient() 传入的文本（默认 "Check connection"）
        inline constexpr unsigned long long CheckConnection = 0ULL;

        /// @brief 服务端空闲超时提醒
        /// @details 会话长时间没收到消息时，服务端先发一条提醒再关闭
        inline constexpr unsigned long long ServerTimeout = 1ULL;

        /// @brief 服务端忙超时提醒
        /// @details 业务回调长时间没回包时，服务端发这条提示后断开连接
        inline constexpr unsigned long long ServerBusyTimeout = 2ULL;

        /// @brief 一条保留 ID 的登记项
        struct Command
        {
            unsigned long long id;   ///< 消息 ID
            std::string_view name;   ///< 名字（日志 / 排查时用）
            std::string_view note;   ///< 语义说明
        };

        // ── 保留区 [0, 999] 的分段 ───────────────────────────────────────────
        // 把整段划成三块，好处是"某个 ID 该归谁管"不靠口头约定：
        //   0–9    库内部命令（目前已用 0/1/2）
        //   10–99  框架 / 网关命令（登录态、幂等键、链路追踪这类横切能力）
        //   100–999 应用自定义特殊命令（业务里全局广播、管理指令等）
        // 1000 起才是普通业务消息。
        inline constexpr unsigned long long kLibraryIdBegin = 0ULL;        ///< 库内部命令起点
        inline constexpr unsigned long long kLibraryIdEnd = 9ULL;          ///< 库内部命令终点
        inline constexpr unsigned long long kFrameworkIdBegin = 10ULL;     ///< 框架命令起点
        inline constexpr unsigned long long kFrameworkIdEnd = 99ULL;       ///< 框架命令终点
        inline constexpr unsigned long long kApplicationIdBegin = 100ULL;  ///< 应用特殊命令起点
        inline constexpr unsigned long long kApplicationIdEnd = 999ULL;    ///< 应用特殊命令终点

        /// @brief 一个 msg_id 所属的区段
        enum class Range
        {
            Library,     ///< [0, 9]    库内部
            Framework,   ///< [10, 99]  框架 / 网关
            Application, ///< [100, 999] 应用自定义特殊命令
            Business,    ///< [1000, ...] 普通业务消息
        };

        /// @brief  判断 msg_id 属于哪个区段
        /// @param  id 消息 ID
        /// @return 所属区段（永远有值，不会"未定义"）
        /// @note   编译期可用；`IsReserved()` 等价于 `Classify(id) != Range::Business`
        constexpr Range Classify(unsigned long long id) noexcept
        {
            if (id <= kLibraryIdEnd)
            {
                return Range::Library;
            }
            if (id <= kFrameworkIdEnd)
            {
                return Range::Framework;
            }
            if (id <= kApplicationIdEnd)
            {
                return Range::Application;
            }
            return Range::Business;
        }

        /// @brief  取区段名字（日志用）
        /// @param  range 区段
        /// @return 固定字符串
        constexpr std::string_view RangeName(Range range) noexcept
        {
            switch (range)
            {
            case Range::Library:
                return "Library";
            case Range::Framework:
                return "Framework";
            case Range::Application:
                return "Application";
            default:
                return "Business";
            }
        }

        /// @brief 已知的保留 ID 清单
        /// @details 新增特殊 ID 时**只在这里加一条**，并同步更新文档；
        ///          这样 `NameOf()` 自动可用，两端也不会各写一份数字
        inline constexpr std::array<Command, 3> kCommands{{
            {CheckConnection, "CheckConnection", "客户端连接成功后自动发送的连接测试"},
            {ServerTimeout, "ServerTimeout", "服务端空闲超时提醒（发完即关闭）"},
            {ServerBusyTimeout, "ServerBusyTimeout", "服务端忙超时提醒（业务长时间未回包）"},
        }};

        /// @brief  判断是否落在保留区间（< 1000）
        /// @param  id 消息 ID
        /// @return 保留区间返回 true
        /// @note   编译期可用（constexpr）
        constexpr bool IsReserved(unsigned long long id) noexcept
        {
            return id < kFirstBusinessMsgId;
        }

        /// @brief  判断是否属于业务区间（>= 1000）
        /// @param  id 消息 ID
        /// @return 业务区间返回 true
        /// @note   编译期可用（constexpr）
        constexpr bool IsBusiness(unsigned long long id) noexcept
        {
            return id >= kFirstBusinessMsgId;
        }

        /// @brief  取消息 ID 的名字
        /// @param  id 消息 ID
        /// @return 已登记的保留 ID 返回其名字；
        ///         未登记的保留 ID 返回 "<reserved>"；业务 ID 返回 "<business>"
        /// @note   仅用于日志与排查，不要拿它做分支判断
        constexpr std::string_view NameOf(unsigned long long id) noexcept
        {
            for (const auto& command : kCommands)
            {
                if (command.id == id)
                {
                    return command.name;
                }
            }
            return IsReserved(id) ? std::string_view{"<reserved>"} : std::string_view{"<business>"};
        }
    } // namespace MsgId
} // namespace Net
