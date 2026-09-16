/// @file        NetServer.h
/// @brief       TCP 服务器与连接会话（Session / Server）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

// 防止重复包含
#pragma once
#include <vector>
#include <csignal>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <tuple>
#include <memory>

#include "NetConnection.h" //Server继承Conection

/// @namespace  Net
/// @brief      网络通讯模块
/// @details    目前实现了TCP连接
/// @note
namespace Net
{
    /// @namespace  Server
    /// @brief      网络通讯服务器子模块
    /// @details    只有TCP连接
    /// @note
    namespace Server
    {

        /// @brief      连接会话
        /// @details    tcp通讯的会话，负责与单个客户端收发数据
        /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
        /// @note
        class Session : public Connection
        {
        public:
            /// @brief 消息推送回调类型
            /// @param session 触发回调的会话智能指针
            /// @param msg_id 消息全局唯一ID
            /// @param msg 消息序列化字符串
            /// @warning 禁止持有session裸指针，必须使用shared_ptr延长生命周期
            /// @note 回调内部禁止长时间阻塞，会阻塞asio事件循环
            using PushMessage = std::function<void(const std::shared_ptr<Session>& session, unsigned long long msg_id,
                                                   const std::string& msg)>;

            /// @brief 消息推送回调

            /// @brief      构造函数
            /// @details session的构造
            /// @param[in] io 连接的io_context
            /// @param[in] sock 连接的socket
            /// @param[in] PMFunction 有消息的回调函数
            /// @warning
            /// @note
            Session(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock,
                    std::unique_ptr<PushMessage> PMFunction);

            /// @brief      安全获取自身智能指针
            /// @details    继承自 Connection，需从基类向下转换为 Session
            /// @return     指向本对象的 shared_ptr
            /// @warning    必须在本对象已被 shared_ptr 管理时调用
            /// @note
            std::shared_ptr<Session> shared_from_this()
            {
                return std::static_pointer_cast<Session>(Connection::shared_from_this());
            }

            /// @brief      回复消息
            /// @details    主线程调用，向该客户端回复一条消息
            /// @param[in] msg_id 消息全局唯一ID
            /// @param[in] msg 消息序列化字符串
            /// @warning    禁止在回调或事件循环中长时间阻塞
            /// @note
            void Reply(unsigned long long msg_id, std::string msg);

            /// @brief      所属的 io_context
            /// @details    保存该会话使用的 io_context 引用
            /// @warning    生命周期须长于本会话
            /// @note
            boost::asio::io_context& ioc;

            /// @brief      业务处理
            /// @details    收到消息后的具体业务实现，由基类回调触发
            /// @param[in] msg_id 消息全局唯一ID
            /// @param[in] msg 消息序列化字符串
            /// @warning    禁止在回调内部长时间阻塞，会阻塞asio事件循环
            /// @note
            void ToWork(unsigned long long msg_id, std::string msg) override;

        private:
            /// @brief 设置收到的回调
            /// @details 设置工作函数的回调
            /// @param[in] CBFunc
            /// @note
            void SetCBFunc(CBFunc);

            /// @brief      停止标志
            /// @details    标识该会话是否已停止
            /// @note
            std::atomic<bool> stop;
        };

        /// @brief      服务器端
        /// @details    负责 listen / accept 的服务器类，用于与客户端建立连接，
        ///             并将会话消息统一投递到消息队列供主线程消费
        /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
        /// @note
        class Server : public std::enable_shared_from_this<Server>
        {
        public:
            /// @brief      构造函数
            /// @details    服务器的构造
            /// @param[in] io 服务器的io_context
            /// @param[in] ep 监听的本地端点（地址与端口）
            /// @warning    须保证 io 的生命周期长于本服务器
            /// @note
            Server(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep);

            /// @brief      开始接受连接
            /// @details    在 io_context 线程中被调用，异步等待并接受客户端连接，
            ///             并为每个新连接创建对应的 Session
            /// @warning    须在 io_context 运行后调用
            /// @note
            void StartAccept();

            /// @brief      停止服务器
            /// @details    停止接受新连接并关闭所有会话
            /// @warning    调用后服务器不再接受新连接，须重新构造使用
            /// @note
            virtual void Stop();

            /// @brief      阻塞等待消息
            /// @details    主线程调用，阻塞等待一条消息
            /// @return     消息元组 {session, msg_id, 内容}
            /// @warning    会阻塞调用线程直至有消息到达或服务器停止
            /// @note
            std::tuple<std::shared_ptr<Session>, unsigned long long, std::string> WaitForMessage();

            /// @brief      检查是否有消息
            /// @details    主线程调用，非阻塞检查消息队列是否非空
            /// @return     存在待处理消息返回 true，否则返回 false
            /// @warning    仅做检查，不会取出消息
            /// @note
            bool HasMessage();

            /// @brief      投递消息到队列
            /// @details    供 Session::ToWork 调用，把消息投递到消息队列并唤醒主线程
            /// @param[in] session 触发消息的会话智能指针
            /// @param[in] msg_id 消息全局唯一ID
            /// @param[in] msg 消息序列化字符串
            /// @warning    须在 io_context 线程中调用
            /// @note
            void PushMessage(const std::shared_ptr<Session>& session, unsigned long long msg_id,
                             const std::string& msg);

        protected:
            /// @brief      所属的 io_context
            /// @details    保存服务器使用的 io_context 引用
            /// @warning    生命周期须长于本服务器
            /// @note
            boost::asio::io_context& ioc;

            /// @brief      运行状态标志
            /// @details    标识服务器当前是否正在运行
            /// @note
            std::atomic<bool> running;

        private:
            /// @brief      监听器
            /// @details    保存用于 listen / accept 的 acceptor
            /// @note
            boost::asio::ip::tcp::acceptor acceptor;
            /// @brief      会话列表
            /// @details    管理所有已建立的连接会话
            /// @note
            std::vector<std::shared_ptr<Session>> sessions;

            /// @brief      消息队列
            /// @details    IO线程生产、主线程消费的消息队列
            /// @note
            std::queue<std::tuple<std::shared_ptr<Session>, unsigned long long, std::string>> msg_queue;
            /// @brief      队列互斥锁
            /// @details    保护消息队列的互斥锁
            /// @note
            std::mutex queue_mutex;
            /// @brief      队列条件变量
            /// @details    消息到达或服务器退出时唤醒主线程
            /// @note
            std::condition_variable queue_cv;

            /// @brief      失效会话清理线程
            /// @details    定期清理失效会话的后台线程
            /// @note
            std::unique_ptr<std::thread> clearsession_thread;
        };
    } // namespace Server
} // namespace Net