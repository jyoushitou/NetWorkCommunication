#include "NetClient.h"

namespace Net
{
    namespace Client
    {

        // 开始函数
        void Client::Start()
        {
            // 发出连接请求
            ToSend("客户端发出连接，是否收到");

            Utils::Out_Msg("客户端发出连接测试请求", serviceID);

            // 等待回复
            Connection::Start();
        }

        // 构造函数
        Client::Client(boost::asio::io_context& io, int serviceID)
            : Connection(boost::asio::ip::tcp::socket(io), serviceID), ioc(io), resolver(io), running(true)
        {
        }

        // 连接服务器端
        void Client::Connect(const std::string& host, const std::string& port)
        {
            // 保活
            auto self = shared_from_this();

            // 使用成员resolver（必须作为成员，保证异步解析期间resolver对象存活）
            resolver.async_resolve(host, port,
                                   [this, self, host](const boost::system::error_code& ec,
                                                      boost::asio::ip::tcp::resolver::results_type endpoints)
                                   {
                                       // 如果有错误
                                       if (ec)
                                       {
                                           Utils::Out_Err("解析地址失败: " + ec.what(), serviceID);
                                           // 通知主线程退出，防止 WaitForMessage 永久阻塞
                                           ToWork(-1ULL, "resolve_failed");
                                           Stop();
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
                                                   Utils::Out_Err("连接失败: " + ec_conect.what(), serviceID);
                                                   return;
                                               }

                                               Utils::Out_Msg(host + "连接成功", serviceID);
                                               Start();
                                           });
                                   });
        }

        void Client::ToWork(unsigned long long msg_id, std::string msg)
        {
            {
                // 加锁放入队列
                std::lock_guard<std::mutex> lock(queue_mutex);
                msg_queue.emplace(msg_id, std::move(msg));
            }
            // 唤醒等待中的主线程
            queue_cv.notify_one();
        }

        // 主线程调用：阻塞等待一条消息
        std::pair<unsigned long long, std::string> Client::WaitForMessage()
        {
            // 加锁
            std::unique_lock<std::mutex> lock(queue_mutex);

            // 等待队列非空或停止信号
            queue_cv.wait(lock, [this]() { return !msg_queue.empty() || !running; });

            // 如果是停止信号且队列为空，返回终止标记
            if (msg_queue.empty())
            {
                return {-1ULL, "close"};
            }

            // 取出队首消息
            auto msg = std::move(msg_queue.front());
            // 弹出队首消息
            msg_queue.pop();
            return msg;
        }

        // 非阻塞检查
        bool Client::HasMessage()
        {
            // 加锁
            std::lock_guard<std::mutex> lock(queue_mutex);
            // 队列为空则返回false，有消息返回true
            return !msg_queue.empty();
        }

        // 停止函数
        void Client::Stop()
        {
            {
                // 加锁
                std::lock_guard<std::mutex> lock(queue_mutex);
                // 判断是否在跑
                running = false;
            }
            // 唤醒主线程，让它退出等待
            queue_cv.notify_all();
            // 关闭连接
            Close();
        }

    } // namespace Client
} // namespace Net