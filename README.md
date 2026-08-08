# NetWorkCommunication

一个基于 Boost.Asio 的 C++ 网络通讯库,采用 `io_context` 单线程事件循环模型,提供异步 TCP 收发与业务逻辑解耦的消息队列接口。

## 目的
- 为WebServer端的通讯模块，由于是内嵌于所有的微服务之间，故分离出此服务通讯架构
- 同时为网络通讯留档

## 特性

- 基于 Boost.Asio 异步 I/O(async_read / async_write)
- 单 `io_context` 事件循环,配合 `boost::asio::post` 实现跨线程安全发送
- 消息头包含 8 字节消息 ID + 4 字节消息长度,支持自定义业务 ID
- 发送队列自动串行化,避免多线程并发写 socket
- 独立的发送/接收缓冲区类(`MsgNode` / `RecvNode` / `SendNode`)
- 客户端与服务端均提供阻塞式 `WaitForMessage()` 消息队列接口,方便业务线程消费
- 内置服务 ID 映射宏(`Message.h`),支持多服务路由扩展

## 环境依赖

- CMake >= 3.16
- C++17
- Boost(asio / system / thread)
- vcpkg(推荐)或系统安装的 Boost

## 目录结构

```
NetWorkCommunication/
├── connon/                  # 公共网络库核心代码
│   ├── Message.h           # 常量定义（消息头长度、服务ID映射等）
│   ├── NetConnection.h     # 连接基类、MsgNode/RecvNode/SendNode 声明
│   ├── NetConnection.cpp   # 异步收发、消息解析、发送队列实现
│   ├── Utils.h             # 日志输出工具声明
│   └── Utils.cpp           # 日志输出工具实现
├── Server/                  # 服务端示例
│   └── source/
│       ├── main.cpp        # 服务端入口，等待并回复消息
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── NetServer.h # Server / Session 声明
│       └── body/
│           └── NetServer.cpp # 接受连接、会话管理、消息分发
└── Client/                  # 客户端示例
    └── source/
        ├── main.cpp        # 客户端入口，发消息并等待回复
        ├── CMakeLists.txt
        ├── include/
        │   └── NetClient.h # Client 声明
        └── body/
            └── NetClient.cpp # 异步连接、消息队列消费
```

## 消息协议

消息头固定 12 字节,网络字节序(Big-Endian):

| 字段     | 长度   | 说明                                                                 |
| -------- | ------ | -------------------------------------------------------------------- |
| 消息 ID  | 8 字节 | 业务消息标识                                                         |
| 消息长度 | 4 字节 | 消息体长度（不含消息头，在程序运行中自动计算消息长度，无需手动输入） |
| 消息体   | 可变长 | 实际业务数据（不超过 1MB，可在Message.h中修改）                      |

## 编译与运行

### 使用 vcpkg 安装 Boost

```bash
vcpkg install boost-asio boost-system boost-thread
```

### 编译服务端

```bash
cd Server/source
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

### 编译客户端

```bash
cd Client/source
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

### 运行

先启动服务端:

```bash
cd Server/build
./Debug/Server.exe
```

再启动客户端:

```bash
cd Client/build
./Debug/Client.exe
```

## 使用示例

### 服务端

```cpp
// 创建 io_context 和监听端点
boost::asio::io_context io;
boost::asio::ip::tcp::endpoint ep(boost::asio::ip::tcp::v4(), 60000);

// 启动服务器
auto server = std::make_shared<Net::Server::Server>(io, ep, ServiceID_RPCGateway);
server->StartAccept();

// 网络线程
std::thread net_thread([&io] { io.run(); });

// 业务主线程：阻塞等待消息
while (true) {
    auto [session, msg_id, msg] = server->WaitForMessage();
    if (msg_id == -1ULL && msg == "close") break;

    std::cout << "收到消息 ID=" << msg_id << " 内容=" << msg << std::endl;

    // 回复客户端
    session->Reply(msg_id, "收到: " + msg);
}

net_thread.join();
```

### 客户端

```cpp
boost::asio::io_context io;

// 创建客户端并连接
auto client = std::make_shared<Net::Client::Client>(io, ServiceID_SQL);
client->Connect("127.0.0.1", "60000");

// 网络线程
std::thread net_thread([&io] { io.run(); });

// 业务主线程：阻塞等待回复
while (true) {
    auto [msg_id, msg] = client->WaitForMessage();
    if (msg_id == -1ULL) break;  // 连接关闭或解析失败

    std::cout << "收到回复 ID=" << msg_id << " 内容=" << msg << std::endl;
    break;
}

net_thread.join();
```

## 设计说明

1. **单线程事件循环**:所有 socket 读写都在 `io_context` 线程执行,业务线程通过 `WaitForMessage()` 阻塞消费消息,二者通过线程安全队列解耦。

2. **跨线程发送**:业务线程调用 `Send()` 时,实际通过 `boost::asio::post` 将发送任务投递到 IO 线程,由 IO 线程串行写入 socket,避免数据竞争。

3. **发送队列**:`send_queue` 保存待发送的消息,`sending` 标志防止并发写;发送完成后自动取出下一条继续发送。

4. **消息缓存类**:`MsgNode` 内部维护 `char* buf`,构造时即分配 `total_len + 1` 字节并将末尾置 `'\0'`,防止字符串越界。

## 已知问题与修复记录

- **构造函数参数顺序错误**(`MsgNode(int, int)`):委托构造时参数位置写反,导致 `max_len` 被传入 `-1ULL` 截断为 `-1`,触发 `max_len 必须大于 0` 错误、缓冲区未分配。已修复为 `MsgNode(-1ULL, max_len, serviceID)`。

## 许可证

MIT License