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
        /// @brief      客户端连接类
        /// @details    继承自 Connection，负责连接服务器、收发消息并回调上层业务
        /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
        /// @note
        class Client : public Connection
        {
        public:
            /// @brief      构造函数
            /// @details    唯一的构造函数
            /// @param[in] io 保存会话的io_context
            /// @param[in] serviceID 服务ID，用于日志打印
            /// @warning    生命周期须长于本客户端
            /// @note
            Client(boost::asio::io_context&, int);

            /// @brief      连接服务器端
            /// @details    异步解析地址并建立连接，成功后自动调用 start()
            /// @param[in] host 服务器地址
            /// @param[in] port 服务器端口
            /// @warning    须在 IO 线程启动前调用
            /// @note
            void Connect(const std::string&, const std::string&);

            /// @brief      开始函数
            /// @details    发出连接测试请求并启动连接的异步读取流程
            /// @warning    须在socket连接建立后调用
            /// @note
            void start() override;

            /// @brief      注册消息回调
            /// @details    收到一条消息时触发，在 IO 线程内被调用
            /// @param[in] cb 消息回调函数（参数为消息ID与消息体）
            /// @warning    禁止在回调中长时间阻塞
            /// @note
            void SetMessageCallback(std::function<void(unsigned long long, std::string)> cb);

            /// @brief      注册关闭回调
            /// @details    连接彻底关闭时触发，在 IO 线程内被调用
            /// @param[in] cb 关闭回调函数
            /// @warning    禁止在回调中长时间阻塞
            /// @note
            void SetCloseCallback(std::function<void()> cb);

            /// @brief      停止函数
            /// @details    从外部线程安全调用，向 IO 线程投递关闭请求，不丢弃已收到的消息
            /// @warning    异步执行，调用后连接不再可用
            /// @note
            void Stop();

        protected:
            /// @brief 后期转到具体业务
            /// @param msg_id 消息id
            /// @param msg 消息体
            /// @warning 必须重写
            /// @note
            void recvToWork(unsigned long long, std::string) override;

            /// @brief 连接关闭通知
            /// @details 连接彻底关闭时回调，触发上层注册的关闭回调
            /// @warning 仅在 IO 线程内被调用，禁止在此长时间阻塞
            /// @note
            void toClosed() override;

        private:
            /// @brief      解析器
            /// @details    用于异步解析服务器地址（必须作为成员，保证异步解析期间对象存活）
            /// @warning    生命周期须长于本客户端
            /// @note
            boost::asio::ip::tcp::resolver resolver;

            /// @brief      消息回调存储
            /// @details    收到消息时调用的上层回调
            /// @note
            std::function<void(unsigned long long, std::string)> message_cb;

            /// @brief      关闭回调存储
            /// @details    连接彻底关闭时调用的上层回调
            /// @note
            std::function<void()> close_cb;
        };
    } // namespace Client
} // namespace Net