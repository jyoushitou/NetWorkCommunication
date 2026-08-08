#pragma once

#include <string>
#include <queue>
#include <memory>

#include "NetConnection.h"
#include "Utils.h"

namespace Net
{
    namespace Client
    {
        class Client : public Connection
        {
        public:
            // 构造函数
            Client(boost::asio::io_context&, int);

            // 连接函数
            void Connect(const std::string&, const std::string&);

            // 主线程调用：阻塞等待一条消息，返回 {msg_id, 内容}
            std::pair<unsigned long long, std::string> WaitForMessage();
            // 主线程调用：非阻塞检查是否有消息
            bool HasMessage();

            // 启动
            void Start() override;

            // 停止函数：不丢弃已收到的消息。
            // 调用后，WaitForMessage() 会先依次返回队列中剩余的 {msg_id, string 消息}，
            // 等队列为空后，再返回 {-1, "close"} 表示连接已关闭。
            void Stop();

        private:
            // IO抛出收到的数据
            void ToWork(unsigned long long, std::string) override;

            // 保存io_context
            boost::asio::io_context& ioc;

            // 解析器（必须作为成员，保证异步解析期间对象存活）
            boost::asio::ip::tcp::resolver resolver;

            // 消息队列（IO线程生产，主线程消费）
            std::queue<std::pair<unsigned long long, std::string>> msg_queue;
            std::mutex queue_mutex;
            std::condition_variable queue_cv;
            std::atomic<bool> running;
        };
    } // namespace Client
} // namespace Net