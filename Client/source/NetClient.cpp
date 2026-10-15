/// @file        NetClient.cpp
/// @brief       网络通讯客户端实现（异步连接、消息回调、优雅停止）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026

#include "NetClient.h"

#include <boost/asio.hpp>

#include "Utils.h"

namespace Net
{
    namespace Client
    {
        /// @brief      开始函数
        /// @details    先发送 ID 为 0 的 "check connection" 测试消息，再调用基类启动异步读取流程
        /// @warning    须在socket连接建立后调用
        /// @note       基类的读取流程幂等（started 标志），但本函数每次调用都会再发一条测试消息
        void Client::start()
        {
            // 发出连接测试请求
            toSend(0, "check connection");

            Utils::Out::outMsg("客户端发出连接测试请求");

            // 启动基类的异步读取流程（回复由消息回调处理）
            Connection::start();
        }

        /// @brief      获取端口和ip
        /// @return     本客户端保存的 HostPort（host + port）
        /// @note
        HostPort Client::getHostPort()
        {
            return HP;
        }

        /// @brief      构造函数
        /// @details    唯一的构造函数：创建裸 socket 交给基类，并保存回调、地址端口与发送上限
        /// @param[in] io 保存会话的io_context
        /// @param[in] HF 消息回调（unique_ptr 转移所有权，可为空）
        /// @param[in] HP 服务器地址与端口
        /// @param[in] CF 解析/连接失败时的业务关闭回调（unique_ptr 转移所有权，可为空）
        /// @param[in] maxSendBufLen 发送队列允许积压的最大字节数
        /// @warning    io 的生命周期须长于本客户端
        /// @note
        Client::Client(boost::asio::io_context& io, std::unique_ptr<HandleFunction> HF, HostPort HP,
                       std::unique_ptr<CloseFunction> CF, int maxSendBufLen)
            : Connection(boost::asio::ip::tcp::socket(io), io, maxSendBufLen), resolver(io)
        {
            // 保存消息回调
            this->HF = std::move(HF);
            // 保存服务器地址与端口
            this->HP = HP;

            // 转移所有权到client
            this->CF = std::move(CF);

            // 关闭通知标记复位
            closeNotifiedByUser = false;
        }

        /// @brief      连接服务器端
        /// @details    用成员 resolver 异步解析 HP.host / HP.port，再 async_connect；
        ///             连接成功后设置 NoDelay 并调用 start()
        /// @warning    须在 io_context.run() 之前调用，否则异步操作无人驱动
        /// @note       解析或连接失败会回调 CloseFunction 并关闭连接
        void Client::Connect()
        {
            // 保活：延长本对象生命周期至异步操作完成
            auto self = shared_from_this();

            /// @brief      异步解析服务器地址
            /// @details    使用成员resolver（必须作为成员，保证异步解析期间resolver对象存活）
            /// @warning    解析期间须保证本对象存活
            /// @note
            resolver.async_resolve(HP.host, HP.port,
                                   [this, self](const boost::system::error_code& ec,
                                                boost::asio::ip::tcp::resolver::results_type endpoints)
                                   {
                                       // 解析失败处理
                                       if (ec)
                                       {
                                           Utils::Out::outErr("解析地址失败: " + ec.what());
                                           // 该路径只负责通知业务侧并关闭连接（主线程的等待由 Utils::Exit 统一唤醒）

                                           // 通知业务侧关闭
                                           talkErrorToWork(ec);

                                           // 关闭连接
                                           errToClose();
                                           return;
                                       }

                                       /// @brief      异步连接
                                       /// @details    使用解析得到的端点建立 TCP 连接
                                       /// @warning    连接期间须保证本对象存活
                                       /// @note
                                       boost::asio::async_connect(socket, endpoints,
                                                                  [this, self](const boost::system::error_code& ec,
                                                                               const boost::asio::ip::tcp::endpoint&)
                                                                  {
                                                                      /// @brief      连接失败处理
                                                                      /// @details    打印错误并关闭连接
                                                                      /// @note
                                                                      if (ec)
                                                                      {
                                                                          Utils::Out::outErr("连接失败: " + ec.what());

                                                                          // 通知业务侧关闭
                                                                          talkErrorToWork(ec);

                                                                          // 关闭连接
                                                                          errToClose();

                                                                          return;
                                                                      }

                                                                      /// @brief      连接成功后启动
                                                                      /// @details    打印日志并启动连接的异步读取流程
                                                                      /// @note
                                                                      Utils::Out::outMsg(this->HP.host + "连接成功");

                                                                      // 开启 TCP_NODELAY（禁用 Nagle，小包立即发送）
                                                                      offNoDelay();

                                                                      start();
                                                                  });
                                   });
        }

        /// @brief      后期转到具体业务
        /// @details    收到完整消息后回调上层注册的消息回调
        /// @param[in] msg_id 消息id
        /// @param[in] msg 消息体
        /// @warning    仅在 IO 线程内被调用，禁止在此长时间阻塞
        /// @note       HF 为空时本条消息被直接丢弃
        void Client::recvToWork(unsigned long long msg_id, std::string msg)
        {
            if (HF)
            {
                (*HF)(msg_id, std::move(msg));
            }
        }

        /// @brief      通知业务侧连接失败
        /// @details    仅由 Connect() 的解析失败 / 连接失败分支调用；用 closeNotifiedByUser 保证只通知一次
        /// @param[in] ec 解析或连接失败的错误码
        /// @note       CF 为空时不通知
        void Client::talkErrorToWork(const boost::system::error_code& ec)
        {
            if (CF && *CF && !closeNotifiedByUser.exchange(true))
            {
                (*CF)(ec);
            }
        }

        /// @brief      关闭时的派生类清理
        /// @details    重写基类纯虚函数；当前为空实现（Client 没有需要额外释放的动态资源）
        /// @note       由基类 socketClose() 保证只回调一次
        void Client::derivedToClose()
        {
        }

        /// @brief      停止函数
        /// @details    从外部线程安全调用，向 IO 线程投递关闭请求，不丢弃发送队列中待发的消息
        /// @warning    异步执行，调用后连接最终断开
        /// @note       内部即基类 stopToClose()，幂等
        void Client::Stop()
        {
            stopToClose();
        }

    } // namespace Client
} // namespace Net