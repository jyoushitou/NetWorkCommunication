#include "NetClient.h"

#include <boost/asio.hpp>

namespace Net
{
    namespace Client
    {

        /// @brief      开始函数
        /// @details    发出连接测试请求并启动连接的异步读取流程
        /// @warning    须在socket连接建立后调用
        /// @note
        void Client::start()
        {
            // 发出连接测试请求
            toSend(0, "check connection");

            Utils::Out::outMsg("客户端发出连接测试请求");

            // 等待回复
            Connection::start();
        }

        /// @brief      构造函数
        /// @details    唯一的构造函数
        /// @param[in] io 保存会话的io_context
        /// @param[in] serviceID 服务ID，用于日志打印
        /// @warning    生命周期须长于本客户端
        /// @note
        Client::Client(boost::asio::io_context& io, int serviceID)
            : Connection(boost::asio::ip::tcp::socket(io), io), resolver(io)
        {
        }

        /// @brief      连接服务器端
        /// @details    异步解析地址并建立连接，成功后自动调用 start()
        /// @param[in] host 服务器地址
        /// @param[in] port 服务器端口
        /// @warning    须在 IO 线程启动前调用
        /// @note
        void Client::Connect(const std::string& host, const std::string& port)
        {
            // 保活：延长本对象生命周期至异步操作完成
            auto self = shared_from_this();

            /// @brief      异步解析服务器地址
            /// @details    使用成员resolver（必须作为成员，保证异步解析期间resolver对象存活）
            /// @warning    解析期间须保证本对象存活
            /// @note
            resolver.async_resolve(host, port,
                                   [this, self, host, port](const boost::system::error_code& ec,
                                                            boost::asio::ip::tcp::resolver::results_type endpoints)
                                   {
                                       // 解析失败处理
                                       if (ec)
                                       {
                                           Utils::Out::outErr("解析地址失败: " + ec.what());
                                           // 通知主线程退出，防止 WaitForMessage 永久阻塞
                                           close();
                                           return;
                                       }

                                       /// @brief      异步连接
                                       /// @details    使用解析得到的端点建立 TCP 连接
                                       /// @warning    连接期间须保证本对象存活
                                       /// @note
                                       boost::asio::async_connect(
                                           socket, endpoints,
                                           [this, self, host](const boost::system::error_code& ec_conect,
                                                              const boost::asio::ip::tcp::endpoint&)
                                           {
                                               /// @brief      连接失败处理
                                               /// @details    打印错误并关闭连接
                                               /// @note
                                               if (ec_conect)
                                               {
                                                   Utils::Out::outErr("连接失败: " + ec_conect.what());
                                                   close();
                                                   return;
                                               }

                                               /// @brief      连接成功后启动
                                               /// @details    打印日志并启动连接的异步读取流程
                                               /// @note
                                               Utils::Out::outMsg(host + "连接成功");
                                               start();
                                           });
                                   });
        }

        /// @brief      注册消息回调
        /// @details    收到一条消息时触发，在 IO 线程内被调用
        /// @param[in] cb 消息回调函数（参数为消息ID与消息体）
        /// @warning    禁止在回调中长时间阻塞
        /// @note
        void Client::SetMessageCallback(std::function<void(unsigned long long, std::string)> cb)
        {
            message_cb = std::move(cb);
        }

        /// @brief      注册关闭回调
        /// @details    连接彻底关闭时触发，在 IO 线程内被调用
        /// @param[in] cb 关闭回调函数
        /// @warning    禁止在回调中长时间阻塞
        /// @note
        void Client::SetCloseCallback(std::function<void()> cb)
        {
            close_cb = std::move(cb);
        }

        /// @brief 后期转到具体业务
        /// @details 收到完整消息后回调上层注册的消息回调
        /// @param msg_id 消息id
        /// @param msg 消息体
        /// @warning 仅在 IO 线程内被调用，禁止在此长时间阻塞
        /// @note
        void Client::recvToWork(unsigned long long msg_id, std::string msg)
        {
            if (message_cb)
            {
                message_cb(msg_id, std::move(msg));
            }
        }

        /// @brief 连接关闭通知
        /// @details 连接彻底关闭时回调，触发上层注册的关闭回调
        /// @warning 仅在 IO 线程内被调用，禁止在此长时间阻塞
        /// @note
        void Client::toClosed()
        {
            if (close_cb)
                close_cb();
        }

        /// @brief      停止函数
        /// @details    从外部线程安全调用，向 IO 线程投递关闭请求，不丢弃已收到的消息
        /// @warning    异步执行，调用后连接不再可用
        /// @note
        void Client::Stop()
        {
            // 基类 close() 内部 post 到 IO 线程，线程安全
            close();
        }

    } // namespace Client
} // namespace Net