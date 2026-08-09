# NetWorkCommunication

一个基于 Boost.Asio 的 C++ 网络通讯库,采用 `io_context` 单线程事件循环模型,提供异步 TCP 收发与业务逻辑解耦的消息队列接口。

## 目的
- 为WebService端的通讯模块，由于是内嵌于所有的微服务之间，故分离出此服务通讯架构
- 同时为网络通讯留档
- WebService链接：[WebService](https://github.com/jyoushitou/WebService)

## 特性

- 基于 Boost.Asio 异步 I/O(async_read / async_write)
- 单 `io_context` 事件循环,配合 `boost::asio::post` 实现跨线程安全发送
- 消息头包含 8 字节消息 ID + 4 字节消息长度,支持自定义业务 ID
- 消息 ID 支持原子自增自动分配(`g_net_msg_id`)与显式指定两种方式
- 跨平台字节序转换(自定义 `htonll` / `ntohll`，兼容 Windows / Linux)
- 发送队列自动串行化,避免多线程并发写 socket
- 独立的发送/接收缓冲区类(`MsgNode` / `RecvNode` / `SendNode`)
- 服务端提供阻塞式 `WaitForMessage()` / 非阻塞式 `HasMessage()` 消息队列接口
- 服务端支持优雅退出：`Stop()` + 信号处理（Ctrl+C / taskkill / 关闭窗口）
- 客户端支持多连接并行（每连接独立 `io_context` + 独立线程)
- 客户端提供回调机制(`SetMessageCallback` / `SetCloseCallback`)接收消息与关闭通知
- 内置服务 ID 映射宏(`Message.h`,支持 1~16 号服务路由扩展)

## 环境依赖

- CMake >= 3.16
- C++17
- Boost(asio / system / thread)
- vcpkg(推荐)或系统安装的 Boost
  - CMakeLists 会自动检测 `VCPKG_ROOT` 环境变量或项目内 `vcpkg/` 目录作为工具链

## 目录结构

```
NetWorkCommunication/
├── connon/                  # 公共网络库核心代码
│   ├── Message.h            # 常量定义（消息头长度、服务ID映射、原子消息ID）
│   ├── NetConnection.h      # 连接基类、MsgNode/RecvNode/SendNode 声明
│   ├── NetConnection.cpp    # 异步收发、字节序转换、消息解析、发送队列实现
│   ├── Utils.h              # 日志输出工具声明
│   └── Utils.cpp            # 日志输出工具实现
├── Server/                  # 服务端示例
│   └── source/
│       ├── main.cpp         # 服务端入口，信号处理/优雅退出/等待并回复消息
│       ├── CMakeLists.txt   # 自动检测 vcpkg 工具链、/MP /FS /utf-8 编译选项
│       ├── include/
│       │   └── NetServer.h  # Server / Session 声明
│       └── body/
│           └── NetServer.cpp # accept、会话管理、线程安全消息队列、WaitForMessage
├── Client/                  # 客户端示例
│   ├── source/
│   │   ├── main.cpp         # 客户端入口，支持多连接、事件机制优雅退出
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── NetClient.h  # Client 声明
│   │   └── body/
│   │       └── NetClient.cpp # 异步连接（成员resolver）、消息回调、关闭回调
├── .gitignore
├── LICENSE
└── README.md
```

## 消息协议

消息头固定 12 字节，网络字节序(Big-Endian)：

| 字段     | 长度   | 说明                                                                        |
| -------- | ------ | --------------------------------------------------------------------------- |
| 消息 ID  | 8 字节 | 业务消息标识（默认原子自增，可显式指定；发送/接收时自动做 64 位字节序转换） |
| 消息长度 | 4 字节 | 消息体长度（不含消息头，在程序运行中自动计算消息长度，无需手动输入）        |
| 消息体   | 可变长 | 实际业务数据（不超过 1MB，可在Message.h中修改）                             |

## 编译与运行

### 使用 vcpkg 安装 Boost

```bash
vcpkg install boost-asio boost-system boost-thread
```

### 设置 vcpkg 工具链（CMakeLists 自动检测）

```bash
# Windows (PowerShell)
$env:VCPKG_ROOT = "C:\path\to\vcpkg"

# Linux / macOS
export VCPKG_ROOT=/path/to/vcpkg
```

> 也可以将 vcpkg 克隆到项目根目录的 `vcpkg/` 子目录下，CMakeLists 同样会自动检测。或者显式指定：`-DCMAKE_TOOLCHAIN_FILE=...`。

### 编译服务端

```bash
cd Server/source
cmake -B build
cmake --build build --config Debug
```

### 编译客户端

```bash
cd Client/source
cmake -B build
cmake --build build --config Debug
```

### 运行

先启动服务端:

```bash
# Windows
cd Server/source/build && ./Debug/Server.exe
# Linux
cd Server/source/build && ./Server
```

再启动客户端:

```bash
# Windows
cd Client/source/build && ./Debug/Client.exe
# Linux
cd Client/source/build && ./Client
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

// 注册 Ctrl+C / Windows 控制台事件，实现优雅退出（见 main.cpp 的 OnSignal / ConsoleCtrlHandler）

// 网络线程
std::thread net_thread([&io] { io.run(); });

// 业务主线程：阻塞等待消息
while (true) {
    auto [session, msg_id, msg] = server->WaitForMessage();
    if (!session && msg == "close") break;   // Stop() 后返回终止标记

    std::cout << "收到消息 ID=" << msg_id << " 内容=" << msg << std::endl;

    // 回复客户端
    session->Reply(msg_id, "收到: " + msg);
}

// 停止服务器（幂等，可安全重复调用）
server->Stop();
net_thread.join();
```

### 客户端

```cpp
boost::asio::io_context io;

// 创建客户端（必须先 Connect 再启动 io 线程）
auto client = std::make_shared<Net::Client::Client>(io, ServiceID_SQL);

// 注册消息回调（在 io_context 线程中执行）
client->SetMessageCallback([](unsigned long long msg_id, std::string msg) {
    std::cout << "收到回复 ID=" << msg_id << " 内容=" << msg << std::endl;
});

// 注册关闭回调
client->SetCloseCallback([]() {
    std::cout << "连接已关闭" << std::endl;
});

// 异步连接（内部 async_resolve + async_connect，resolver 为成员保证生命周期）
client->Connect("127.0.0.1", "60000");

// 网络线程（必须在 Connect 之后启动，保证 io_context 中已有异步任务）
std::thread net_thread([&io] { io.run(); });

// 业务线程：通过 ToSend() 发送消息（线程安全，内部 post 到 IO 线程）
std::thread send_thread([&client] {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") break;
        client->ToSend(line);   // 隐式分配消息 ID；也可 ToSend(msg_id, line) 显式指定
    }
});

net_thread.join();
send_thread.join();
```

### 客户端多连接（14 业务服务并行）

`Client/source/main.cpp` 内置了多连接架构：

- 每连接一个独立 `io_context` + 一个独立 `io_thread`（共 18 条内网连接，默认注释，可取消注释启用）
- 连接关闭时通过 `SetCloseCallback` 统计剩余连接数，全部关闭后自动退出

```cpp
// 创建 18 条内网连接
for (size_t i = 0; i < 18; ++i) {
    CreateConnection(i, "127.0.0.1", "60000");
}
```

## 设计说明

1. **单线程事件循环**：所有 socket 读写都在 `io_context` 线程执行，业务线程通过 `WaitForMessage()` 阻塞消费消息，二者通过线程安全队列（`mutex` + `condition_variable`）解耦。

2. **跨线程发送**：业务线程调用 `Send()` 时，实际通过 `boost::asio::post` 将发送任务投递到 IO 线程，由 IO 线程串行写入 socket，避免数据竞争。

3. **发送队列**：`send_queue` 保存待发送的消息，`sending` 标志防止并发写；发送完成后自动取出下一条继续发送。`Close()` 请求后若队列仍有数据会先发送完毕再关闭（优雅关闭）。

4. **消息缓存类**：`MsgNode` 内部维护 `char* buf`，构造时即分配 `total_len + 1` 字节并将末尾置 `'\0'`，防止字符串越界。带 ID 的节点构造为后续日志追踪提供支持。

5. **跨平台字节序**：自定义 `htonll` / `ntohll` 实现 64 位网络字节序转换（Windows 无原生实现），32 位长度用 `htonl` / `ntohl`，保证协议跨平台一致。

6. **原子消息 ID**：`g_net_msg_id` 使用 `std::atomic<unsigned long long>` 自增分配，多线程调用 `ToSend(msg)` 也不会产生重复 ID；需要追踪时可显式指定 `ToSend(msg_id, msg)`。

7. **优雅退出**：
   - 服务端：`Stop()` 关闭 acceptor、通知所有 Session 停止、唤醒 `WaitForMessage()` 返回终止标记，并支持 SIGINT / Windows 控制台事件处理。
   - 客户端：Windows 下使用事件对象（`CreateEvent` + `WaitForSingleObject`）挂起主线程，Ctrl+C 时由系统回调线程仅设置事件唤醒主线程，主线程再安全地执行 `Stop()` → `join()` 清理流程（避免在系统回调线程中调用 `Stop()` 的线程安全问题）。

## 已知问题与修复记录

- **构造函数参数顺序错误**(`MsgNode(int, int)`):委托构造时参数位置写反，导致 `max_len` 被传入 `-1ULL` 截断为 `-1`，触发 `max_len 必须大于 0` 错误、缓冲区未分配。已修复为 `MsgNode(-1ULL, max_len, serviceID)`。

- **客户端 io_context 线程提前退出**(`Client/source/main.cpp` 的 `CreateConnection`):原代码先启动 `io_thread` 执行 `conn->io->run()`，但此时 `io_context` 中没有任何异步任务，`run()` 立即返回，线程随之结束；之后才调用 `Connect()`，导致 `async_resolve` 排入队列却无人驱动，连接永远不会建立，控制台无任何输出。已修复为先调用 `Connect()` 再启动 `io_thread`。

- **Windows winsock 头文件冲突**:`winsock.h` 与 `winsock2.h` 冲突导致编译报错。已在两个 CMakeLists 中统一添加 `WIN32_LEAN_AND_MEAN` 与 `_WIN32_WINNT=0x0601` 编译宏。

- **Windows 控制台 UTF-8 中文乱码**:`Utils::init()` 调用 `SetConsoleOutputCP(CP_UTF8)` 设置输出代码页，CMake 添加 `/utf-8` 编译选项保证源文件按 UTF-8 解析。

- **解析器生命周期问题**(`Client::Connect`):`async_resolve` 的 resolver 原为临时局部变量，异步解析期间对象销毁导致崩溃/未定义行为。已改为 `Client` 成员变量（`boost::asio::ip::tcp::resolver resolver;`）。

- **Windows 控制台关闭时崩溃**:原在 `ConsoleCtrlHandler` 系统回调线程中直接调用 `Stop()`（内部 `boost::asio::post`），存在竞态风险。已改为回调线程只设置 `g_exit_event` 事件唤醒主线程，由主线程统一执行优雅关闭流程。

- **关闭回调重复触发**:`ActuallyClose()` 可能被 `ReadHead` 错误、`ReadBody` 错误、`Close()` 等多次调用路径触发。已添加 `close_notified` 标记，保证 `ToClosed()` 只被调用一次。

- **关闭后继续读消息**:连接正在关闭(`closing=true`)时，`ReadHead` 解析到的消息直接丢弃并立即 `ActuallyClose()`，避免对已关闭连接继续发起异步读。

## 许可证

MIT License