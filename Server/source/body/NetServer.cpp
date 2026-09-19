/// @file        NetServer.cpp
/// @brief       TCP 服务器与连接会话实现（Session / Server）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026
/// Distributed under the MIT License. See LICENSE file.

//
#include "NetServer.h"

#include "Utils.h"

namespace Net
{
    namespace Server
    {
        /// @brief      连接会话
        /// @details    tcp通讯的会话，负责与单个客户端收发数据
        /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
        /// @note
        Session::Session(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock,
                         std::shared_ptr<HandleFunction> HF)
            : Connection(std::move(sock)), ioc(io)
        {
            // 初始化停止状态置为false
            stop = false;
            // 给回调函数赋值
            this->HF = HF;
        }

        /// @brief      工作函数（读取到消息后的业务处理）
        /// @details    输出消息并把消息投递到服务器的队列，等待主线程处理
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息序列化字符串
        /// @warning
        /// @note
        void Session::toWork(unsigned long long msg_id, std::string msg)
        {
            // 输出收到的消息
            Utils::Out::outNetMsg(msg_id, "收到客户端消息:" + msg);

            // 获得自身指针
            auto self = shared_from_this();

            // 存储回调函数数组
            std::string sendmsg;

            // 判断是否获得了回调函数指针
            if (HF != nullptr)
            {
                sendmsg = (this->HF)(self, msg);
            }

            reply(msg_id, sendmsg);
        }

        // 主线程调用：向该客户端回复一条消息
        void Session::reply(unsigned long long msg_id, const std::string& msg)
        {

            Utils::Out::outMsg("发送回复数据");
            toSend(msg_id, std::move(msg));
        }

        /// @brief 关闭session
        /// @details 供sever关闭线程关闭
        void Session::closeSession()
        {
            // 使用基类的关闭函数
            close();
        }

        /// @brief 更新连接时间

        void Session::updateTime()
        {
            time_t lastTime = Utils::Time::nowTime();
        }

        //===Server===
        Server::Server(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep)
            : ioc(io), acceptor(io), running(true)
        {
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
                                          auto session = std::make_unique<Session>(ioc, sock, HF);
                                          sessions.push_back(session);
                                          // 启动读（继承自 Connection::start()）
                                          session->start();

                                          // 继续接受下一个连接
                                          StartAccept();
                                      }
                                      else
                                      {
                                          // 仅在服务器仍在运行时，才输出真正的 accept 错误
                                          Utils::Out::outErr("accept 错误: " + ec.what());
                                      }
                                  });
        }

        // 停止函数
        void Server::Stop()
        {
            {
                // 加锁
                std::lock_guard<std::mutex> lock(queueMutex);
                // 标记停止
                running = false;
            }
            // 唤醒主线程，让它退出等待
            queueCV.notify_all();

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
        void Server::PushMessage(const std::shared_ptr<Session>& session, unsigned long long msg_id,
                                 const std::string& msg)
        {
            {
                // 加锁放入队列
                std::lock_guard<std::mutex> lock(queueMutex);
                msgQueue.emplace(session, msg_id, std::move(msg));
            }
            // 唤醒等待中的主线程
            queueCV.notify_one();
        }

        // 主线程调用：阻塞等待一条消息
        std::tuple<std::shared_ptr<Session>, unsigned long long, std::string> Server::WaitForMessage()
        {
            // 加锁
            std::unique_lock<std::mutex> lock(queueMutex);

            // 等待队列非空或停止信号
            queueCV.wait(lock, [this]() { return !msgQueue.empty() || !running; });

            // 如果是停止信号且队列为空，返回终止标记
            if (msgQueue.empty())
            {
                return {nullptr, -1ULL, "close"};
            }

            // 取出队首消息
            auto msg = std::move(msgQueue.front());
            // 弹出队首消息
            msgQueue.pop();
            return msg;
        }

        // 非阻塞检查
        bool Server::HasMessage()
        {
            // 加锁
            std::lock_guard<std::mutex> lock(queueMutex);
            // 队列为空则返回false，有消息返回true
            return !msgQueue.empty();
        }
    } // namespace Server
} // namespace Net