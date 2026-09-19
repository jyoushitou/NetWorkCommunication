#include "NetClient.h"

namespace Net
{
    namespace Client
    {

        // 开始函数
        void Client::start()
        {
            // 发出连接请求
            toSend("客户端发出连接，是否收到");

            Utils::outMsg("客户端发出连接测试请求", serviceID);

            // 等待回复
            Connection::start();
        }

        // 构造函数
        Client::Client(boost::asio::io_context& io, int serviceID)
            : Connection(boost::asio::ip::tcp::socket(io), serviceID), ioc(io), resolver(io)
        {
        }

        // 连接服务器端
        void Client::Connect(const std::string& host, const std::string& port)
        {
            // 保活
            auto self = getSelfThis();

            // 使用成员resolver（必须作为成员，保证异步解析期间resolver对象存活）
            resolver.async_resolve(host, port,
                                   [this, self, host, port](const boost::system::error_code& ec,
                                                            boost::asio::ip::tcp::resolver::results_type endpoints)
                                   {
                                       // 如果有错误
                                       if (ec)
                                       {
                                           Utils::outErr("解析地址失败: " + ec.what(), serviceID);
                                           // 通知主线程退出，防止 WaitForMessage 永久阻塞
                                           close();
                                           return;
                                       }

                                       // 异步连接
                                       boost::asio::async_connect(
                                           sock, endpoints,
                                           [this, self, host](const boost::system::error_code& ec_conect,
                                                              const boost::asio::ip::tcp::endpoint&)
                                           {
                                               if (ec_conect)
                                               {
                                                   Utils::outErr("连接失败: " + ec_conect.what(), serviceID);
                                                   close();
                                                   return;
                                               }

                                               Utils::outMsg(host + "连接成功", serviceID);
                                               start();
                                           });
                                   });
        }

        // 注册消息回调
        void Client::SetMessageCallback(std::function<void(unsigned long long, std::string)> cb)
        {
            message_cb = std::move(cb);
        }

        // 注册关闭回调
        void Client::SetCloseCallback(std::function<void()> cb)
        {
            close_cb = std::move(cb);
        }

        // 给工作任务
        void Client::toWork(unsigned long long msg_id, std::string msg)
        {
            if (message_cb)
            {
                message_cb(msg_id, std::move(msg));
            }
        }

        // 连接彻底关闭，触发关闭回调
        void Client::toClosed()
        {
            if (close_cb)
                close_cb();
        }

        // Stop：从外部线程安全调用
        void Client::Stop()
        {
            // 基类 close() 内部 post 到 IO 线程，线程安全
            close();
        }

    } // namespace Client
} // namespace Net