#pragma once
#include <vector>
#include <csignal>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <tuple>

#include "NetConnection.h"
#include "Utils.h"

namespace Net
{
    namespace Server
    {
        // 前置声明
        class Server;

        // 连接类
        // 负责与客户端通讯
        class Session : public Connection
        {
        public:
            // 构造
            Session(boost::asio::io_context&, boost::asio::ip::tcp::socket, int serviceID, Server* server);

            // 安全获取自身 shared_ptr（继承自 Connection，需从基类转换）
            std::shared_ptr<Session> shared_from_this()
            {
                return std::static_pointer_cast<Session>(Connection::shared_from_this());
            }

            // 停止函数
            void Stop();

            // 主线程调用：向该客户端回复一条消息
            void Reply(unsigned long long msg_id, std::string msg);

        protected:
            // 保存ioc
            boost::asio::io_context& ioc;

            // 业务实现
            void ToWork(unsigned long long, std::string) override;

        private:
            // 停止
            std::atomic<bool> stop;
            // 所属服务器
            Server* server;
        };

        // 服务器端
        // 负责 listen / accept 的服务器类
        // 用于与客户端连接
        class Server : public std::enable_shared_from_this<Server>
        {
        public:
            // 构造函数
            Server(boost::asio::io_context&, boost::asio::ip::tcp::endpoint, int serviceID);

            // 开始接受连接（在 io_context 线程中被调用）
            void StartAccept();

            // 停止接受新连接并关闭所有会话
            virtual void Stop();

            // 主线程调用：阻塞等待一条消息，返回 {session, msg_id, 内容}
            std::tuple<std::shared_ptr<Session>, unsigned long long, std::string> WaitForMessage();
            // 主线程调用：非阻塞检查是否有消息
            bool HasMessage();

            // 接收来自 HTTP 的前端数据（Vue3）
            void PushHttpRequest(std::string body);

            // 处理 Vue3 数据的业务函数（可自己扩展）
            // 以后所有 Vue 请求都会走到这里
            std::string HandleVueRequest(const std::string& path, const std::string& body);

            // 供 Session::ToWork 调用：把消息投递到队列
            void PushMessage(const std::shared_ptr<Session>&, unsigned long long, std::string);

        protected:
            // 保存io_context
            boost::asio::io_context& ioc;

            // 保存当前服务器ID
            int serviceID;

            // 运行线程的保存
            std::atomic<bool> running;

        private:
            // 保存acceptor
            boost::asio::ip::tcp::acceptor acceptor;

            // 管理连接对话
            std::vector<std::shared_ptr<Session>> sessions;

            // 消息队列（IO线程生产，主线程消费）
            std::queue<std::tuple<std::shared_ptr<Session>, unsigned long long, std::string>> msg_queue;
            std::mutex queue_mutex;
            std::condition_variable queue_cv;
        };
    } // namespace Server
} // namespace Net