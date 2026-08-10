#pragma once
#include <memory>
#include <string>
#include <cstring>
#include <deque>

#include "Message.h"
#include "Utils.h"

#include <cstdint>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <boost/asio.hpp>

namespace Net
{
    // 消息体缓存
    // 为读取发送构造逻辑层缓存
    class MsgNode
    {
    public:
        // 构造消息体（只有长度）
        MsgNode(int, int);
        // 构造消息体（有长度和消息ID）
        MsgNode(unsigned long long, int, int);
        // 析构消息体
        virtual ~MsgNode();

        // 清空缓存
        void Clear();
        // 设置当前读取位置
        void SetCurLen(int);
        // 设置消息ID
        void SetID(unsigned long long);

        // 获取缓存区指针
        char* GetBuf() const;
        // 获取读取位置
        int GetCurLen() const;
        // 获取缓存区总长度
        int GetTotalLen() const;
        // 获取消息ID
        unsigned long long GetID() const;

        // 禁用拷贝构造函数
        MsgNode(const MsgNode&) = delete;
        // 禁用拷贝赋值函数
        MsgNode& operator=(const MsgNode&) = delete;

    protected:
        // 消息体缓存
        char* buf;
        // 缓存数据大小
        int total_len;
        // 当前读取到的位置
        int cur_len;
        // 消息ID
        unsigned long long msg_id;
    };

    // 读取类
    class RecvNode : public MsgNode
    {
    public:
        // 长度ID构造
        RecvNode(unsigned long long, int, int);
        // 长度构造
        RecvNode(int, int);

    private:
    };

    class SendNode : public MsgNode
    {
    public:
        SendNode(unsigned long long, int, int);

    private:
    };

    // 连接基类
    // 读取发送的实现函数
    class Connection : public std::enable_shared_from_this<Connection>
    {
    public:
        // 唯一构造函数
        explicit Connection(boost::asio::ip::tcp::socket, int);

        // 发送任务创建
        // 隐式msg_id
        // 一般创建发送函数
        void ToSend(const std::string&);
        // 发送任务创建
        // 显式指定 msg_id
        // 日志追踪用
        void ToSend(unsigned long long msg_id, const std::string&);

        // 开始函数
        virtual void Start();

        // 取消默认析构函数
        ~Connection() = default;

    protected:
        // 关闭连接
        void Close();

        // 连接真正关闭后的回调（派生类可重写，IO线程内触发）
        virtual void ToClosed();

        // 给业务逻辑层函数
        virtual void ToWork(unsigned long long, std::string);

        // 发送消息到发送队列
        void Send(unsigned long long msg_id, std::string msg);

        // socket关闭
        void ActuallyClose();

        // 保存服务器ID
        int serviceID;
        // 存储socket
        boost::asio::ip::tcp::socket sock;

    private:
        // 发送消息
        void DoSend();

        // 读取头部
        void ReadHead();
        // 读取消息体
        void ReadBody(unsigned long long, int);

        // 存储读取缓存
        std::shared_ptr<RecvNode> recv_node;
        // 发送队列
        std::deque<std::shared_ptr<SendNode>> send_queue;

        // 判断是否在发送
        bool sending;
        // 判断关闭状态
        bool closing;

        // 防止 ActuallyClose 重复触发回调的标记
        bool close_notified = false;
    };
} // namespace Net