/// @file        Message.h
/// @brief       服务ID定义与网络协议常量
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// 防止重复包含
#pragma once
#include <unordered_map>
#include <string>
#include <atomic>

/// @brief      RPC网关服务
/// @details    服务器ID=1，负责请求转发与路由
/// @note
constexpr int ServiceID_RPCGateway = 1;
/// @brief      SQL数据库服务
/// @details    服务器ID=2，负责数据持久化
/// @note
constexpr int ServiceID_SQL = 2;
/// @brief      注册中心服务
/// @details    服务器ID=3，负责服务发现与注册
/// @note
constexpr int ServiceID_Registry = 3;
/// @brief      配置中心服务
/// @details    服务器ID=4，负责统一配置管理
/// @note
constexpr int ServiceID_ConfigCenter = 4;
/// @brief      监控服务
/// @details    服务器ID=5，负责系统运行状态监控
/// @note
constexpr int ServiceID_MonitorService = 5;
/// @brief      安全服务
/// @details    服务器ID=6，负责访问控制与安全防护
/// @note
constexpr int ServiceID_SecurityService = 6;
/// @brief      证书服务
/// @details    服务器ID=7，负责证书签发与管理
/// @note
constexpr int ServiceID_CertService = 7;
/// @brief      链路追踪服务
/// @details    服务器ID=8，负责分布式链路追踪
/// @note
constexpr int ServiceID_TracingService = 8;
/// @brief      服务控制台
/// @details    服务器ID=9，负责服务管理界面
/// @note
constexpr int ServiceID_ServiceConsole = 9;
/// @brief      管理控制台
/// @details    服务器ID=10，负责后台管理界面
/// @note
constexpr int ServiceID_AdminConsole = 10;
/// @brief      用户服务
/// @details    服务器ID=11，负责用户信息与认证
/// @note
constexpr int ServiceID_User = 11;
/// @brief      文章服务
/// @details    服务器ID=12，负责文章内容管理
/// @note
constexpr int ServiceID_Article = 12;
/// @brief      博客服务
/// @details    服务器ID=13，负责博客业务逻辑
/// @note
constexpr int ServiceID_Blog = 13;
/// @brief      图片服务
/// @details    服务器ID=14，负责图片上传与处理
/// @note
constexpr int ServiceID_Image = 14;
/// @brief      视频服务
/// @details    服务器ID=15，负责视频上传与处理
/// @note
constexpr int ServiceID_Video = 15;
/// @brief      搜索服务
/// @details    服务器ID=16，负责全文检索
/// @note
constexpr int ServiceID_Search = 16;

/// @brief      服务器ID映射
/// @details    服务器ID到服务名称的映射表
/// @note
inline std::unordered_map<int, std::string> ServiceID = {
    {1, "RPCGateway"},      {2, "SQL"},         {3, "Registry"},       {4, "ConfigCenter"},   {5, "MonitorService"},
    {6, "SecurityService"}, {7, "CertService"}, {8, "TracingService"}, {9, "ServiceConsole"}, {10, "AdminConsole"},
    {11, "User"},           {12, "Article"},    {13, "Blog"},          {14, "Image"},         {15, "Video"},
    {16, "Search"}};

namespace Net
{
    /// @brief      消息ID长度
    /// @details    消息头部中消息ID所占字节数
    /// @note
    constexpr short HEAD_ID_LENGTH = 8;

    /// @brief      消息长度长度
    /// @details    消息头部中消息长度字段所占字节数
    /// @note
    constexpr short HEAD_LEN_LENGTH = 4;

    /// @brief      消息头部长度
    /// @details    消息ID、消息长度、请求服务器ID与目标服务器ID长度之和
    /// @note
    constexpr short HEAD_LENGTH = HEAD_ID_LENGTH + HEAD_LEN_LENGTH;

    /// @brief      最大消息长度
    /// @details    单条消息允许的最大长度（1M）
    /// @warning    超出该长度视为非法消息
    /// @note
    constexpr int MAX_LENGTH = 1024 * 1024;
} // namespace Net