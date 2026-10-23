/// @file        NetClient.h
/// @brief       网络通讯客户端（异步连接、消息回调、优雅停止）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026
#pragma once

#include <string>
#include <queue>
#include <memory>
#include <functional>

#include "NetConnection.h"

namespace Net
{
    namespace Client
    {

        /// @brief 存储ip和端口
        /// @details host 为服务器地址（IP 或域名），port 为端口字符串，两者都交给 async_resolve 解析
        /// @note
        struct HostPort
        {
            std::string host;
            std::string port;
        };

        /// @brief 业务回调类型
        /// @details 收到完整消息后回调（消息ID, 消息体）；在 IO 线程中执行，禁止长时间阻塞
        /// @note
        using HandleFunction = std::function<void(unsigned long long, std::string)>;

        /// @brief 业务关闭回调类型
        /// @details 仅在解析地址或建立连接失败时回调一次（参数为错误码），供业务侧处理退出；
        ///          正常关闭不会触发
        /// @note
        using CloseFunction = std::function<void(const boost::system::error_code)>;

        /// @brief      客户端连接类
        /// @details    继承自 Connection，负责连接服务器、收发消息并回调上层业务
        /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
        /// @warning    同一个 io_context 只允许由一个线程 run()；推荐每个 Client 配一个独立 io_context + IO 线程
        /// @note       连接成功后 start() 会先发送一条 ID 为 0 的 "check connection" 测试消息
        class Client : public Connection
        {
        public:
            /// @brief      构造函数
            /// @details    唯一的构造函数：创建裸 socket 交给基类，并保存回调、地址端口与发送上限
            /// @param[in] io 保存会话的io_context
            /// @param[in] HF 消息回调（unique_ptr 转移所有权，可为空）
            /// @param[in] HP 服务器地址与端口
            /// @param[in] CF 解析/连接失败时的业务关闭回调（unique_ptr 转移所有权，可为空）
            /// @param[in] maxSendBufLen 发送队列允许积压的最大字节数
            /// @warning    io 的生命周期须长于本客户端
            /// @note
            explicit Client(boost::asio::io_context& io, std::unique_ptr<HandleFunction> HF, HostPort HP,
                            std::unique_ptr<CloseFunction> CF, int maxSendBufLen);

            /// @brief      连接服务器端
            /// @details    用成员 resolver 异步解析 HP.host / HP.port，再 async_connect；
            ///             连接成功后设置 NoDelay 并调用 start()
            /// @warning    须在 io_context.run() 之前调用，否则异步操作无人驱动
            /// @note       解析或连接失败会回调 CloseFunction 并关闭连接
            void Connect();

            /// @brief      开始函数
            /// @details    先发送 ID 为 0 的 "check connection" 测试消息，再调用基类启动异步读取流程
            /// @warning    须在socket连接建立后调用
            /// @note       基类的读取流程幂等（started 标志），但本函数每次调用都会再发一条测试消息
            void start() override;

            /// @brief      停止函数
            /// @details    从外部线程安全调用，向 IO 线程投递关闭请求，不丢弃已发送队列中的消息
            /// @warning    异步执行，调用后连接最终断开
            /// @note       内部即基类 stopToClose()，幂等
            void stop();

            /// @brief      获取端口和ip
            /// @return     本客户端保存的 HostPort（host + port）
            /// @note
            HostPort getHostPort();

        protected:
            /// @brief      关闭时的派生类清理
            /// @details    重写基类纯虚函数；当前为空实现（Client 没有需要额外释放的动态资源）
            /// @note       由基类 socketClose() 保证只回调一次
            void derivedToClose() override;

            /// @brief      后期转到具体业务
            /// @param[in] msg_id 消息id
            /// @param[in] msg 消息体
            /// @warning    必须重写；仅在 IO 线程内被调用
            /// @note       HF 为空时本消息被直接丢弃
            void recvToWork(unsigned long long, std::string) override;

        private:
            /// @brief      通知业务侧连接失败
            /// @details    仅由 Connect() 的解析失败 / 连接失败分支调用；用 closeNotifiedByUser 保证只通知一次
            /// @param[in] ec 解析或连接失败的错误码
            /// @note       CF 为空时不通知
            void talkErrorToWork(const boost::system::error_code& ec);

            /// @brief      解析器
            /// @details    用于异步解析服务器地址；必须作为成员（不能是局部变量），否则异步解析期间对象已销毁
            /// @warning    Connect() 的异步解析完成前，本客户端不得析构
            /// @note
            boost::asio::ip::tcp::resolver resolver;

            /// @brief      消息回调存储
            /// @details    收到消息时调用的上层回调
            /// @note       在 IO 线程内被调用
            std::unique_ptr<HandleFunction> HF;

            /// @brief 记录对应的IP和port
            /// @details 记录这个 Client 对应的 host 和 port，供 Connect() 解析使用
            HostPort HP;

            /// @brief 记录业务的关闭回调
            /// @details 解析地址或建立连接失败时通知业务侧（可为空）
            std::unique_ptr<CloseFunction> CF;

            /// @brief 关闭通知标记
            /// @details 保证 CloseFunction 只被回调一次（exchange 判定）
            /// @note
            std::atomic<bool> closeNotifiedByUser = false;
        };
    } // namespace Client
} // namespace Net