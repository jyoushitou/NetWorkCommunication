#include "NetServer.h"

namespace Net
{
    namespace Server
    {
        //===Session===
        // 构造函数
        Session::Session(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock, int serviceID_,
                         Server* server_)
            : Connection(std::move(sock), serviceID_), ioc(io), stop(false), server(server_)
        {
        }

        // 停止函数
        void Session::Stop()
        {
            // 跨线程投递关闭操作到 io_context 线程
            auto self = shared_from_this();
            boost::asio::post(ioc,
                              [this, self]()
                              {
                                  stop = true;
                                  Close();
                              });
        }

        // 工作函数
        void Session::ToWork(unsigned long long msg_id, std::string msg)
        {
            // 输出收到的消息
            Utils::Out_Net_Msg(msg_id, "收到客户端消息:" + msg, serviceID);

            // 把消息投递到服务器的队列，等待主线程处理
            if (server)
            {
                server->PushMessage(shared_from_this(), msg_id, std::move(msg));
            }
        }

        // 主线程调用：向该客户端回复一条消息
        void Session::Reply(unsigned long long msg_id, std::string msg)
        {
            Utils::Out_Msg("处理完成回复消息中", serviceID);
            Send(msg_id, std::move(msg));
        }

        //===Server===
        Server::Server(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep, int serviceID_)
            : ioc(io), acceptor(io), running(true)
        {
            // 给service赋值
            serviceID = serviceID_;

            // 打开连接
            acceptor.open(ep.protocol());
            // 设置
            acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
            // 绑定ep
            acceptor.bind(ep);
            // 监听
            acceptor.listen();
        }

        // accept连接
        void Server::StartAccept()
        {
            // 保活 Server 自身
            auto self = shared_from_this();

            // 创建一个 socket 用于 accept
            auto sock = std::make_shared<boost::asio::ip::tcp::socket>(ioc);

            // 异步连接
            acceptor.async_accept(*sock,
                                  [this, self, sock](boost::system::error_code ec)
                                  {
                                      // 服务器已停止：acceptor 被主动关闭导致的取消错误（如 system:995）
                                      // 是正常退出流程的一部分，不是真正的错误，静默返回即可
                                      if (!running)
                                          return;

                                      if (!ec)
                                      {
                                          // 为每个连接创建一个 Session（传入 this 指针以便消息投递到 Server 队列）
                                          auto session =
                                              std::make_shared<Session>(ioc, std::move(*sock), serviceID, this);
                                          sessions.push_back(session);
                                          // 启动读（继承自 Connection::Start()）
                                          session->Start();

                                          // 继续接受下一个连接
                                          StartAccept();
                                      }
                                      else
                                      {
                                          // 仅在服务器仍在运行时，才输出真正的 accept 错误
                                          Utils::Out_Err("accept 错误: " + ec.what(), serviceID);
                                      }
                                  });
        }

        // 停止函数
        void Server::Stop()
        {
            {
                // 加锁
                std::lock_guard<std::mutex> lock(queue_mutex);
                // 标记停止
                running = false;
            }
            // 唤醒主线程，让它退出等待
            queue_cv.notify_all();

            // 保活
            auto self = shared_from_this();
            // 跨线程输入
            boost::asio::post(ioc,
                              [this, self]()
                              {
                                  // 关闭连接
                                  boost::system::error_code ec;
                                  acceptor.close(ec);

                                  // 循环通知每个会话关闭
                                  for (auto& session : sessions)
                                  {
                                      session->Stop();
                                  }
                                  // 清理会话
                                  sessions.clear();
                              });
        }

        // 投递消息到队列（IO线程调用）
        void Server::PushMessage(const std::shared_ptr<Session>& session, unsigned long long msg_id, std::string msg)
        {
            {
                // 加锁放入队列
                std::lock_guard<std::mutex> lock(queue_mutex);
                msg_queue.emplace(session, msg_id, std::move(msg));
            }
            // 唤醒等待中的主线程
            queue_cv.notify_one();
        }

        // 主线程调用：阻塞等待一条消息
        std::tuple<std::shared_ptr<Session>, unsigned long long, std::string> Server::WaitForMessage()
        {
            // 加锁
            std::unique_lock<std::mutex> lock(queue_mutex);

            // 等待队列非空或停止信号
            queue_cv.wait(lock, [this]() { return !msg_queue.empty() || !running; });

            // 如果是停止信号且队列为空，返回终止标记
            if (msg_queue.empty())
            {
                return {nullptr, -1ULL, "close"};
            }

            // 取出队首消息
            auto msg = std::move(msg_queue.front());
            // 弹出队首消息
            msg_queue.pop();
            return msg;
        }

        // 非阻塞检查
        bool Server::HasMessage()
        {
            // 加锁
            std::lock_guard<std::mutex> lock(queue_mutex);
            // 队列为空则返回false，有消息返回true
            return !msg_queue.empty();
        }
    } // namespace Server
} // namespace Net