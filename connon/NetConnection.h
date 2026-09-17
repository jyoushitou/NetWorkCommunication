/// @file        NetConnection.h
/// @brief       网络通讯连接核心（消息缓存、读写、发送队列）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026
#pragma once
#include <memory>
#include <string>
#include <deque>

#include "Message.h"
#include "Utils.h"

namespace Net
{
    /// @brief      消息体缓存
    /// @details    为读取发送构造逻辑层缓存
    /// @note
    class MsgNode
    {
    public:
        /// @brief      构造函数（只有长度）
        /// @details    仅指定缓存长度，消息ID默认为无效值
        /// @param[in] max_len 缓存的最大长度
        /// @warning    禁止传非正数
        /// @note
        MsgNode(int max_len);

        /// @brief      构造函数（有消息ID和长度）
        /// @details    指定消息ID与缓存长度
        /// @param[in] msg_id_ 消息全局唯一ID
        /// @param[in] max_len 缓存的最大长度
        /// @warning    禁止传非正数
        /// @note
        MsgNode(unsigned long long msg_id_, int max_len);

        /// @brief      析构函数
        /// @details    释放消息体缓存
        /// @note
        virtual ~MsgNode();

        /// @brief      清空缓存
        /// @details    将缓存内容置零并复位读取位置
        /// @note
        void Clear();

        /// @brief      设置当前读取位置
        /// @param[in] len 当前读取到的位置
        /// @note
        void SetCurLen(int len);

        /// @brief      设置消息ID
        /// @param[in] msg_id_ 消息全局唯一ID
        /// @note
        void SetID(unsigned long long msg_id_);

        /// @brief      获取缓存区指针
        /// @return     指向缓冲区首地址的指针
        /// @warning    禁止释放或越界访问
        /// @note
        char* GetBuf() const;

        /// @brief      获取当前读取位置
        /// @return     当前读取位置
        /// @note
        int GetCurLen() const;

        /// @brief      获取缓存区总长度
        /// @return     缓冲区总长度
        /// @note
        int GetTotalLen() const;

        /// @brief      获取消息ID
        /// @return     消息全局唯一ID
        /// @note
        unsigned long long GetID() const;

        /// @brief      禁用拷贝构造函数
        /// @details    消息体缓存独占资源，禁止拷贝
        /// @note
        MsgNode(const MsgNode&) = delete;

        /// @brief      禁用拷贝赋值函数
        /// @details    消息体缓存独占资源，禁止拷贝赋值
        /// @note
        MsgNode& operator=(const MsgNode&) = delete;

    protected:
        /// @brief      消息体缓存
        /// @details    存储消息数据的缓冲区
        /// @warning    由本类管理，禁止外部释放
        /// @note
        char* buf;

        /// @brief      缓存数据大小
        /// @details    缓冲区的总长度
        /// @note
        int total_len;

        /// @brief      当前读取位置
        /// @details    当前已读取/写入到的位置
        /// @note
        int cur_len;

        /// @brief      消息ID
        /// @details    消息全局唯一ID
        /// @note
        unsigned long long msg_id;
    };

    /// @brief      接收节点
    /// @details    继承自 MsgNode，用于读取数据时构造的逻辑层缓存
    /// @note
    class RecvNode : public MsgNode
    {
    public:
        /// @brief      构造函数（有消息ID和长度）
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] max_len 缓存的最大长度
        /// @note
        RecvNode(unsigned long long msg_id, int max_len);

        /// @brief      构造函数（只有长度）
        /// @param[in] max_len 缓存的最大长度
        /// @note
        RecvNode(int max_len);

    private:
    };

    /// @brief      发送节点
    /// @details    继承自 MsgNode，用于发送数据时构造的逻辑层缓存
    /// @note
    class SendNode : public MsgNode
    {
    public:
        /// @brief      构造函数
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] max_len 缓存的最大长度
        /// @note
        SendNode(unsigned long long msg_id, int max_len);

    private:
    };

    /// @brief      连接基类
    /// @details    负责读取、发送数据的实现，派生类可重写业务逻辑
    /// @warning    禁止持有裸指针，必须通过 shared_ptr 管理生命周期
    /// @note
    class Connection : public std::enable_shared_from_this<Connection>
    {
    public:
        /// @brief      构造函数
        /// @details    唯一的构造函数
        /// @param[in] socket 连接的socket
        /// @note
        explicit Connection(boost::asio::ip::tcp::socket socket);

        /// @brief      发送任务创建
        /// @details    显式指定 msg_id，用于日志追踪
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息序列化字符串
        /// @warning    禁止传入空消息或超长消息
        /// @note
        void ToSend(unsigned long long msg_id, const std::string& msg);

        /// @brief      开始函数
        /// @details    启动连接的异步读取流程
        /// @warning    须在socket连接建立后调用
        /// @note
        virtual void Start();

        /// @brief      析构函数
        /// @details    采用默认析构
        /// @note
        ~Connection() = default;

    protected:
        /// @brief      关闭连接
        /// @details    向 IO 线程投递关闭请求，允许外部线程调用
        /// @warning    异步执行，调用后连接不再可用
        /// @note
        void Close();

        /// @brief      连接关闭回调
        /// @details    连接真正关闭后的回调，由派生类重写，IO线程内触发
        /// @warning    禁止在此回调中长时间阻塞
        /// @note
        virtual void ToClosed();

        /// @brief      业务处理函数
        /// @details    给业务逻辑层调用，派生类可重写
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息序列化字符串
        /// @note
        virtual void ToWork(unsigned long long msg_id, std::string msg);

        /// @brief      发送消息到发送队列
        /// @details    把消息封装为发送任务并加入发送队列，线程安全，可在外部线程调用
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息序列化字符串
        /// @note
        void Send(unsigned long long msg_id, std::string msg);

        /// @brief      关闭socket
        /// @details    关闭socket并处理发送队列，IO线程内调用
        /// @warning    内部使用，禁止外部直接调用
        /// @note
        void ActuallyClose();

        /// @brief      存储socket
        /// @details    连接使用的TCP socket
        /// @note
        boost::asio::ip::tcp::socket socket;

    private:
        /// @brief      发送消息
        /// @details    从发送队列取出任务并异步发送，IO线程内调用
        /// @note
        void DoSend();

        /// @brief      读取头部
        /// @details    异步读取消息头部（ID与长度）
        /// @note
        void ReadHead();

        /// @brief      读取消息体
        /// @details    异步读取消息体内容
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg_len 消息体长度
        /// @note
        void ReadBody(unsigned long long msg_id, int msg_len);

        /// @brief      读取缓存
        /// @details    存储当前读取的接收节点
        /// @note
        std::shared_ptr<RecvNode> recv_node;

        /// @brief      发送队列
        /// @details    待发送任务的队列
        /// @note
        std::deque<std::shared_ptr<SendNode>> send_queue;

        /// @brief      发送状态
        /// @details    判断当前是否正在发送
        /// @note
        bool sending;

        /// @brief      关闭状态
        /// @details    判断当前是否处于关闭流程
        /// @note
        bool closing;

        /// @brief      关闭通知标记
        /// @details    防止 ActuallyClose 重复触发回调
        /// @note
        bool close_notified = false;
    };
} // namespace Net