#pragma once

#include <string>
#include <queue>
#include <memory>
#include <functional>

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

            // 启动
            void Start() override;

            // 注册消息回调（收到一条消息，IO线程内被调用）
            void SetMessageCallback(std::function<void(unsigned long long, std::string)> cb);

            // 注册关闭回调（连接彻底关闭时，IO线程内被调用）
            void SetCloseCallback(std::function<void()> cb);

            // 停止函数：不丢弃已收到的消息。
            void Stop();

        protected:
            // IO抛出收到的数据
            void ToWork(unsigned long long, std::string) override;

            // 回调关闭
            void ToClosed() override;

        private:
            // 保存io_context
            boost::asio::io_context& ioc;

            // 解析器（必须作为成员，保证异步解析期间对象存活）
            boost::asio::ip::tcp::resolver resolver;

            // 回调存储
            std::function<void(unsigned long long, std::string)> message_cb;
            std::function<void()> close_cb;
        };
    } // namespace Client
} // namespace Net