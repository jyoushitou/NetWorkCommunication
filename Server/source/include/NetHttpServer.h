// NetHttpServer.h
#pragma once

#include <vector>
#include <functional>
#include <atomic>
#include <chrono>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include "Utils.h"
#include "NetConnection.h"
#include "NetServer.h"

namespace Net
{
    namespace Server
    {
        namespace HttpServer
        {
            // 所有的业务回调函数类型
            using VueCallBack = std::function<std::string(std::shared_ptr<HttpSession> session, common::header head)>;

            // HTTP 会话：继承 Session
            class HttpSession : public Session
            {
            public:
                HttpSession(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock,
                            std::shared_ptr<VueCallBack> VueCallBackFunction);

                // 安全获取自身 unique_ptr（重载返回 HttpSession 类型，避免 protected 访问问题）
                std::shared_ptr<HttpSession> shared_from_this();

                // 重写 Start
                // 不读二进制头，改为读 HTTP 请求
                void Start() override;

                // 发送 HTTP 响应给前端（必须在 io_context 线程内调用）
                void HttpSendResponse(const std::string& body);

                // 线程安全异步发送：任何线程均可调用，内部 post 到 HTTP io_context 线程执行
                void AsyncSendResponse(const std::string& body);

                // 关闭连接
                void Stop();

                // 最后一次处理时间
                long long LastActiveSecond();

                // 是否在忙
                bool busing();

                // 是否正在处理请求（避免清理线程误杀正在等待微服务返回的会话）
                std::atomic<bool> busy{false};

            private:
                // 读取请求体
                void ReadBody();
                // 处理请求（解析 body 并回复）
                void HandleRequest();
                // 处理路由
                void Router(const std::string method, std::string target);
                // 更新最近活动时间
                void UpdateActiveTime();

                // 最近一次活动时间（steady_clock 秒）
                std::atomic<long long> last_active_second{0};

                // 判断是否在忙
                bool busy = false;

                // 回调函数
                std::shared_ptr<VueCallBack> VueCallBackFunction = nullptr;

                // 存储要发的头文件
                common::header head;
            };

            // Http接收服务端
            class HttpServer : public Server
            {
            public:
                // 构造函数（额外传入 http_port 用于 Vue 前端）
                HttpServer(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep, unsigned short http_port_);

                // 注册回调
                void SetVueCallBack(VueCallBack cb);

                // 开始接受 HTTP 连接
                void StartHttpAccept();

                // 重写 Stop：同时关闭 HTTP acceptor
                void Stop() override;

                // 处理 Vue3 请求，返回 JSON 响应字符串
                std::string HandleVueRequest(std::shared_ptr<HttpSession> session);

                // 移除并关闭指定会话（io_context 线程内调用）
                void RemoveSession(std::shared_ptr<HttpSession> session);

            private:
                // 清理空闲超时的 HTTP 会话
                // 清理线程调用
                // 内部post 到 io_context 线程执行）
                void CleanupIdleSessions(long long idle_timeout_ms);

                // HTTP 监听器
                boost::asio::ip::tcp::acceptor http_acceptor;
                // HTTP 端口
                unsigned short http_port;
                // HTTP会话集合
                std::vector<std::unique_ptr<HttpSession>> http_sessions;

                // 存储回调函数
                // 后面统一分发
                std::shared_ptr<VueCallBack> VueCallBackFunction;
            };
        } // namespace HttpServer
    } // namespace Server
} // namespace Net