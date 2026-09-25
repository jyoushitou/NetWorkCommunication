/// @file        NetServer.cpp
/// @brief       TCP 服务器与连接会话实现（Session / Server）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026
/// Distributed under the MIT License. See LICENSE file.

//
#include "NetServer.h"

#include "Utils.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace Net
{
    namespace Server
    {
        /// @brief      构造函数
        /// @details    session的构造
        /// @param[in] io 连接的io_context
        /// @param[in] sock 连接的socket
        /// @param[in] HF 消息回调函数
        /// @param[in] timeout 超时时间
        /// @note 默认60s超时
        Session::Session(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock,
                         std::shared_ptr<HandleFunction> HF, long long timeout)
            : Connection(std::move(sock), io)
        {
            // 初始化停止状态置为false
            stop = false;
            // 给回调函数赋值
            this->HF = HF;
            // 给timeout赋值初始化为60s
            this->timeout = timeout ? timeout : 60;
        }

        /// @brief      业务处理
        /// @details    收到消息后的具体业务实现，由基类回调触发：输出消息、
        ///             调用回调函数并把响应回复给客户端
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息序列化字符串
        /// @warning    禁止在回调内部长时间阻塞，会阻塞asio事件循环
        void Session::recvToWork(const unsigned long long msg_id, std::string msg)
        {
            // 输出收到的消息
            Utils::Out::outNetMsg(msg_id, "收到客户端消息:" + msg);

            // 获得自身指针
            auto self = getSession();

            // 存储回调函数返回的响应
            std::string sendmsg;

            // 判断是否获得了回调函数指针
            if (HF != nullptr)
            {
                sendmsg = (*HF)(self, msg);
            }
            else
            {
                Utils::Out::outErr("未设置消息回调函数，忽略本次消息");
                return;
            }

            // 空消息会导致 toSend 抛出异常（toSend 禁止空串），这里直接跳过
            if (sendmsg.empty())
            {
                Utils::Out::outMsg("回调返回空响应，跳过发送");
                return;
            }

            reply(msg_id, sendmsg);
        }

        /// @brief      获取自身的指针
        /// @details    获取可以用于向上转型的自身指针
        std::shared_ptr<Session> Session::getSession()
        {
            return std::static_pointer_cast<Session>(shared_from_this());
        }

        /// @brief      回复消息
        /// @details    主线程调用，向该客户端回复一条消息
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息序列化字符串
        /// @warning    禁止在回调或事件循环中长时间阻塞
        void Session::reply(unsigned long long msg_id, const std::string& msg)
        {

            Utils::Out::outMsg("发送回复数据");

            // 发送数据
            toSend(msg_id, std::move(msg));
        }

        /// @brief      关闭函数
        /// @details    用于关闭session连接，供服务器关闭线程调用
        void Session::closeSession()
        {
            // 使用基类的关闭函数
            close();
        }

        /// @brief      更新连接时间
        /// @details    用于更新时间，超时自动关闭连接，避免占线
        /// @note       在操作后记得调用此函数刷新时间
        void Session::updateTime()
        {
            // 更新成员变量 lastTime（不能写成局部变量，否则只是遮蔽，等于没更新）
            lastTime = Utils::Time::nowTime();
        }

        /// @brief 判断是否超时
        /// @return
        bool Session::timeOut()
        {
            return timeout < Utils::Time::computeTime(lastTime);
        }

        /// @brief      是否已关闭
        /// @details    判断连接是否已进入关闭流程或 socket 已断开
        /// @return     已关闭返回 true，否则返回 false
        /// @warning    仅在 IO 线程内调用，避免跨线程读取非原子状态
        /// @note
        bool Session::isClosed()
        {
            return stop.load() || closing;
        }

        //===Server===
        /// @brief      构造函数
        /// @details    服务器的构造，完成 acceptor 的 open / set_option / bind / listen
        /// @param[in] io 服务器的io_context
        /// @param[in] ep 监听的本地端点（地址与端口）
        /// @warning    须保证 io 的生命周期长于本服务器
        Server::Server(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep,
                       std::shared_ptr<HandleFunction> HF)
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
            // 给智能指针赋值
            this->HF = HF;

            // 创建监控线程
            // clearSession 是非静态成员函数，必须显式绑定 this，否则 std::thread 无法推导可调用对象
            clearSessionThread = std::thread(&Server::clearSession, this);
        }

        /// @brief      析构函数
        /// @details    停止服务器并回收清理线程，避免 std::thread 对象析构时仍 joinable 触发 std::terminate
        /// @note
        Server::~Server()
        {
            // 先触发停止：置 running=false 并关闭 acceptor / 会话
            Stop();

            // 回收清理线程（Stop 已置 running=false，线程会自行退出循环）
            if (clearSessionThread.joinable())
            {
                clearSessionThread.join();
            }
        }

        /// @brief      开始接受连接
        /// @details    在 io_context 线程中被调用，异步等待并接受客户端连接，
        ///             并为每个新连接创建对应的 Session
        /// @warning    须在 io_context 运行后调用
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
                                          // Session 继承自 enable_shared_from_this，必须用 shared_ptr 管理
                                          // 构造函数按值接收 socket，需 std::move(*sock)
                                          auto session = std::make_shared<Session>(ioc, std::move(*sock), HF, timeOut);
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

        /// @brief      清理失效会话
        /// @details    投递到 io_context 线程内，移除空指针会话（供监控线程调用）
        void Server::clearSession()
        {
            // 保活：post 的 lambda 必须捕获 self，否则 Server 可能在使用前被析构
            auto self = shared_from_this();
            // 在关闭时结束进程
            while (running)
            {
                // 60s轮询
                std::this_thread::sleep_for(std::chrono::seconds(60));
                // post 到 io_context，在 IO 线程内清理失效会话（避免跨线程直接改 sessions）
                boost::asio::post(ioc,
                                  [this, self]()
                                  {
                                      for (auto i : sessions)
                                      {
                                          if (i->timeOut() && !i->isClosed())
                                          {
                                              i->reply(-1, "长时间未连接，已自动关闭");
                                              i->closeSession();
                                          }
                                      }
                                  });
            }
        }

        /// @brief      停止服务器
        /// @details    停止接受新连接并关闭所有会话
        /// @warning    调用后服务器不再接受新连接，须重新构造使用
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

                                  // 循环通知每个会话关闭（用 size_t 避免有符号/无符号比较告警）
                                  for (std::size_t i = 0; i < sessions.size(); ++i)
                                  {
                                      sessions[i]->closeSession();
                                  }

                                  // 清理会话
                                  sessions.clear();
                              });
        }

        /// @brief      阻塞等待消息
        /// @details    主线程调用，阻塞等待一条消息
        /// @return     消息元组 {session, msg_id, 内容}
        /// @warning    会阻塞调用线程直至有消息到达或服务器停止
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

    } // namespace Server
} // namespace Net