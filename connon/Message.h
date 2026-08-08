#pragma once
#include <unordered_map>
#include <string>
#include <atomic>

// 服务器ID
// RPC网关服务（服务器ID=1），负责请求转发与路由
constexpr int ServiceID_RPCGateway = 1;
// SQL数据库服务（服务器ID=2），负责数据持久化
constexpr int ServiceID_SQL = 2;
// 注册中心服务（服务器ID=3），负责服务发现与注册
constexpr int ServiceID_Registry = 3;
// 配置中心服务（服务器ID=4），负责统一配置管理
constexpr int ServiceID_ConfigCenter = 4;
// 监控服务（服务器ID=5），负责系统运行状态监控
constexpr int ServiceID_MonitorService = 5;
// 安全服务（服务器ID=6），负责访问控制与安全防护
constexpr int ServiceID_SecurityService = 6;
// 证书服务（服务器ID=7），负责证书签发与管理
constexpr int ServiceID_CertService = 7;
// 链路追踪服务（服务器ID=8），负责分布式链路追踪
constexpr int ServiceID_TracingService = 8;
// 服务控制台（服务器ID=9），负责服务管理界面
constexpr int ServiceID_ServiceConsole = 9;
// 管理控制台（服务器ID=10），负责后台管理界面
constexpr int ServiceID_AdminConsole = 10;
// 用户服务（服务器ID=11），负责用户信息与认证
constexpr int ServiceID_User = 11;
// 文章服务（服务器ID=12），负责文章内容管理
constexpr int ServiceID_Article = 12;
// 博客服务（服务器ID=13），负责博客业务逻辑
constexpr int ServiceID_Blog = 13;
// 图片服务（服务器ID=14），负责图片上传与处理
constexpr int ServiceID_Image = 14;
// 视频服务（服务器ID=15），负责视频上传与处理
constexpr int ServiceID_Video = 15;
// 搜索服务（服务器ID=16），负责全文检索
constexpr int ServiceID_Search = 16;

// 服务器ID映射
inline std::unordered_map<int, std::string> ServiceID = {
    {1, "RPCGateway"},      {2, "SQL"},         {3, "Registry"},       {4, "ConfigCenter"},   {5, "MonitorService"},
    {6, "SecurityService"}, {7, "CertService"}, {8, "TracingService"}, {9, "ServiceConsole"}, {10, "AdminConsole"},
    {11, "User"},           {12, "Article"},    {13, "Blog"},          {14, "Image"},         {15, "Video"},
    {16, "Search"}};

namespace Net
{
    // 消息ID长度
    constexpr int HEAD_ID_LENGTH = 8;
    // 消息长度长度
    constexpr int HEAD_LEN_LENGTH = 4;
    // 消息头部长度
    constexpr int HEAD_LENGTH = HEAD_ID_LENGTH + HEAD_LEN_LENGTH;
    // 最大消息长度（1M）
    constexpr int MAX_LENGTH = 1024 * 1024;

    // 消息ID
    inline std::atomic<unsigned long long> g_net_msg_id{0};
} // namespace Net