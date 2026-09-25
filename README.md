# NetWorkCommunication

![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![Boost.Asio](https://img.shields.io/badge/Boost-Asio-orange)
![Boost.Beast](https://img.shields.io/badge/Boost-Beast-yellow)
![CMake](https://img.shields.io/badge/build-CMake-brightgreen)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

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
- **HTTP 服务器支持（Boost.Beast）**：基于 `HttpServer` / `HttpSession` 派生类，可同时监听 TCP 二进制协议端口与 HTTP 端口，直接对接 Vue3 前端
- 客户端支持多连接并行（每连接独立 `io_context` + 独立线程)
- 客户端提供回调机制(`SetMessageCallback` / `SetCloseCallback`)接收消息与关闭通知
- 内置服务 ID 映射宏(`Message.h`,支持 1~16 号服务路由扩展)
- `Common`/`Utils`/`Net` 分层命名，构建期别名与安装后导出名一致，支持 `find_package(CommonNet)` 复用

## 环境依赖

- CMake >= 3.16
- C++17
- Boost(asio / system / thread / beast)
- vcpkg(推荐)或系统安装的 Boost（CMakeLists 自动检测 `VCPKG_ROOT`，或用 `-DCMAKE_TOOLCHAIN_FILE=...` 指定）

## 目录结构

```
NetWorkCommunication/
├── connon/                  # 公共网络库核心代码（提交）
│   ├── Message.h            # 常量定义（消息头长度、服务ID映射、原子消息ID）
│   ├── NetConnection.h      # 连接基类、MsgNode/RecvNode/SendNode 声明
│   ├── NetConnection.cpp    # 异步收发、字节序转换、消息解析、发送队列实现
│   ├── Utils.h              # 日志输出工具声明
│   └── Utils.cpp            # 日志输出工具实现
├── Server/                  # 服务端库源码（提交）
│   └── source/
│       ├── include/
│       │   └── NetServer.h      # Server / Session 声明
│       └── body/
│           └── NetServer.cpp    # accept、会话管理、线程安全消息队列、WaitForMessage
├── Client/                  # 客户端库源码（提交）
│   └── source/
│       ├── include/
│       │   └── NetClient.h  # Client 声明
│       └── body/
│           └── NetClient.cpp # 异步连接（成员resolver）、消息回调、关闭回调
├── examples/                # 示例程序入口（提交）
│   ├── CMakeLists.txt       # Server / Client 示例可执行文件定义
│   ├── Server/
│   │   └── main.cpp         # 服务端示例入口，信号处理/优雅退出/业务回调
│   └── Client/
│       └── main.cpp         # 客户端示例入口，支持多连接、事件机制优雅退出
├── cmake/
│   └── CommonNetConfig.cmake.in  # 安装包配置模板（find_package(CommonNet) 用）（提交）
├── lib/                     # 预留目录：本地第三方预编译库/依赖，一般不提交
│                            #   （空目录 Git 不追踪，需要保留可放 lib/.gitkeep）
├── build/                   # 构建产物目录（cmake -B build），不提交，已由 .gitignore 忽略
├── out/                     # IDE（VS/CMake 集成）输出目录，不提交，建议加入 .gitignore
├── CMakeLists.txt           # 根构建入口：统一生成 CommonUtils/CommonNetCore/CommonNetServer/CommonNetClient（提交）
├── .gitignore               # 忽略 build/out/*.lib/*.exe 等产物（提交）
├── LICENSE
└── README.md
```

> **提交约定**：`connon/`、`Server/`、`Client/`、`examples/`、`cmake/`、`CMakeLists.txt`、`README.md`、`LICENSE`、`.gitignore` 为源码与构建脚本，需要提交；`build/`、`out/`、`lib/`（第三方二进制）、以及 `*.lib/*.exe/*.pdb` 等编译产物仅本地保留，不提交。

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
vcpkg install boost-asio boost-system boost-thread boost-beast
```

### 设置 vcpkg 工具链（CMakeLists 自动检测）

```bash
# Windows (PowerShell)
$env:VCPKG_ROOT = "C:\path\to\vcpkg"

# Linux / macOS
export VCPKG_ROOT=/path/to/vcpkg
```

> 也可以将 vcpkg 克隆到项目根目录的 `vcpkg/` 子目录下，CMakeLists 同样会自动检测。或者显式指定：`-DCMAKE_TOOLCHAIN_FILE=...`。

### 编译（根目录统一构建）

所有库与示例都由**根目录 `CMakeLists.txt` 统一生成**，不再单独进入 `Server/`、`Client/` 子目录编译（子目录内已不保留独立 `CMakeLists.txt`）。

```bash
# 在项目根目录执行
cmake -B build
cmake --build build --config Debug
```

生成的目标：

| 目标                | 类型       | 说明                                                                             |
| ------------------- | ---------- | -------------------------------------------------------------------------------- |
| `CommonUtils`       | 静态库     | 日志/通用工具（`connon/Utils.cpp`）                                              |
| `CommonNetCore`     | 静态库     | 协议 / 连接核心（`connon/NetConnection.cpp`），链接 `CommonUtils`                |
| `CommonNetServer`   | 静态库     | 服务端（`Server/source/body/NetServer.cpp`），链接 `CommonNetCore`               |
| `CommonNetClient`   | 静态库     | 客户端（`Client/source/body/NetClient.cpp`），链接 `CommonNetCore`               |
| `Server` / `Client` | 可执行文件 | 示例程序，位于构建目录 `examples/` 下（`-DCOMMONNET_BUILD_EXAMPLES=OFF` 可关闭） |

> **命名层级**：`Common`(大功能) → `Utils` / `Net`(小功能) → `Core` / `Server` / `Client`(具体库)。
> **CMake 别名**（构建期别名与安装后导出名一致）：`Common::Utils`、`Common::Net::Core`、`Common::Net::Server`、`Common::Net::Client`。
> **依赖链**：`CommonUtils` ← `CommonNetCore` ←（`CommonNetServer` / `CommonNetClient`）。

### 安装与在其他项目中使用

```bash
# 安装（头文件与库导出为 CommonNet 包）
cmake --install build --prefix <安装目录>
```

安装后在别的工程里引用：

```cmake
find_package(CommonNet REQUIRED)
target_link_libraries(你的目标 PRIVATE Common::Net::Core)
```

### 运行

先启动服务端（示例可执行文件位于构建目录 `examples/` 下）：

```bash
# Windows
./build/examples/Debug/Server.exe
# Linux
./build/examples/Server
```

再启动客户端：

```bash
# Windows
./build/examples/Debug/Client.exe
# Linux
./build/examples/Client
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

### HTTP 服务器（Vue3 前端接入）

```cpp
// 创建 HTTP 服务器：同时监听 TCP 二进制协议端口 60000 与 HTTP 端口 8080
boost::asio::io_context io;
boost::asio::ip::tcp::endpoint ep(boost::asio::ip::tcp::v4(), 60000);

// 创建 HttpServer 实例（继承 Server，额外传入 HTTP 端口）
auto http_server = std::make_shared<Net::Server::HttpServer::HttpServer>(io, ep, ServiceID_RPCGateway, 8080);

// 开始接收 HTTP 请求（Vue3 前端）
http_server->StartHttpAccept();

// （可选）如果还要接收原生 TCP 客户端，取消注释下面这行：
// http_server->StartAccept();

// 网络线程
std::thread io_thread([&io] { io.run(); });

// 主线程等待退出标志（Ctrl+C / taskkill / 关闭窗口触发优雅退出）
while (!g_exit_flag) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// 优雅退出：同时关闭 TCP 与 HTTP 监听器
http_server->Stop();
io_thread.join();
```

`HttpServer` / `HttpSession` 位于 `Server/source/include/NetHttpServer.h` 与 `Server/source/body/NetHttpServer.cpp`，基于 Boost.Beast 实现 HTTP 解析，`HandleVueRequest()` 可重写以处理 Vue3 前端的请求并返回 JSON 响应。

### 客户端多连接（14 业务服务并行）

`examples/Client/main.cpp` 内置了多连接架构：

- 每连接一个独立 `io_context` + 一个独立 `io_thread`（共 18 条内网连接，默认注释，可取消注释启用）
- 连接关闭时通过 `SetCloseCallback` 统计剩余连接数，全部关闭后自动退出

```cpp
// 创建 18 条内网连接
for (size_t i = 0; i < 18; ++i) {
    CreateConnection(i, "127.0.0.1", "60000");
}
```

## 自定义业务模块指引

本库只负责「消息怎么传」，不关心「消息传什么」。业务模块可以完全复用这套通讯架构，只需关注四件事：**服务 ID、业务处理函数、消息内容协议、Vue3 HTTP 路由**。

### 1. 注册新的服务 ID（Message.h）

每个业务服务分配一个唯一 ID（1~16 已占用，新业务从 17 开始扩展）：

```cpp
// connon/Message.h
// 订单服务（服务器ID=17），负责订单业务
constexpr int ServiceID_Order = 17;
// 支付服务（服务器ID=18），负责支付业务
constexpr int ServiceID_Pay = 18;
// ... 继续扩展
```

> 服务 ID 同时用于日志标识（`Utils::Out_Msg(..., serviceID)`）和服务路由。

### 2. 服务端业务处理（Server 的每个独立业务模块）

每个业务服务 = 一份 `Server` 目录拷贝 + 修改 `ServiceID` + 在 `main.cpp` 的 `WaitForMessage()` 循环中编写业务逻辑：

```cpp

// examples/Server/main.cpp —— 在 TODO 注释处编写你的业务处理逻辑
while (true) {
    auto [session, msg_id, msg] = server->WaitForMessage();
    if (!session && msg == "close") break;   // Stop() 后返回终止标记

    Utils::Out_Msg("收到客户端消息[id=" + std::to_string(msg_id) + "]: " + msg, ServiceID_);

    // ===== 自定义业务逻辑开始 =====
    // 方式一：按消息 ID 路由（推荐，msg_id 可作为业务命令字）
    switch (msg_id) {
        case 1001: {   // 1001 = 查询订单
            std::string reply = HandleQueryOrder(msg);   // 你的业务函数
            session->Reply(msg_id, reply);
            break;
        }
        case 1002: {   // 1002 = 创建订单
            std::string reply = HandleCreateOrder(msg);  // 你的业务函数
            session->Reply(msg_id, reply);
            break;
        }
        default:
            session->Reply(msg_id, "未知命令，请检查消息 ID");
            break;
    }
    // ===== 自定义业务逻辑结束 =====
}
```

新业务模块的推荐步骤：

1. 复制 `Server/` 目录为 `OrderService/`（或直接在仓库中新增子目录）；
2. 修改 `main.cpp` 中创建 `Server` 时的 `ServiceID` 为你注册的新 ID；
3. 在 `WaitForMessage()` 循环中扩展现有的 `switch` 分支；
4. 若同时需要 HTTP 前端，调用 `RunHttpServer()` 而不是 `RunServer()`。

### 3. 客户端接入（Client）

客户端对每个业务服务建立一条连接，业务逻辑写在 `SetMessageCallback` 回调中（在 io 线程执行，注意回调中不要做阻塞操作）：

```cpp
// 创建订单服务客户端
auto order_client = std::make_shared<Net::Client::Client>(io, ServiceID_Order);

order_client->SetMessageCallback(
    [](unsigned long long msg_id, std::string msg) {
        std::cout << "[订单服务] 收到回复 ID=" << msg_id << " 内容=" << msg << std::endl;
    });

order_client->Connect("127.0.0.1", "60001");   // 订单服务端口

// 发送带业务命令字的消息（显式指定消息 ID = 业务命令字）
order_client->ToSend(1001, "查询订单: 20240001");
```

### 4. HTTP 业务扩展（Vue3 前端）

`HttpServer::HandleVueRequest()` 是所有 Vue 请求的入口，在 `Server/source/body/NetHttpServer.cpp` 中扩展路由即可：

```cpp
// Server/source/body/NetHttpServer.cpp
std::string HttpServer::HandleVueRequest(const std::string& path, const std::string& body)
{
    Utils::Out_Msg("[Vue请求] path=" + path + ", body=" + body, serviceID);

    // ===== 自定义路由开始 =====
    if (path == "/api/order/get") {
        return "{\"code\":200, \"data\":{\"orderId\":\"20240001\"}}";
    }
    if (path == "/api/order/list") {
        return "{\"code\":200, \"data\":[{\"orderId\":\"20240001\"}, {\"orderId\":\"20240002\"}]}";
    }
    // 其他路由...
    // ===== 自定义路由结束 =====

    return "{\"code\":404, \"msg\":\"route not found\"}";
}
```

### 5. 消息 ID 使用约定（建议）

| 场景                       | 消息 ID 用法                                        |
| -------------------------- | --------------------------------------------------- |
| 无需区分的简单透传         | 省略 ID，由 `g_net_msg_id` 原子自增                 |
| 需要命令字区分业务         | `ToSend(1001, data)` 显式指定，服务端按 ID `switch` |
| 需要追踪一条消息的完整链路 | 发送端记录分配的 ID，服务端用同一 ID 回复           |

按上述步骤，新增一个业务服务只需改动「服务 ID + 处理逻辑」两处，网络收发、队列、线程模型、优雅退出完全复用。

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

8. **HTTP 服务器集成**：`HttpServer` 继承 `Server`，在同一个 `io_context` 中同时运行 TCP 二进制协议监听器与 HTTP 监听器。`HttpSession` 继承 `Session` 并重写 `Start()`，通过 Boost.Beast 的 `request_parser` 解析 HTTP 请求，解析完成后调用 `HandleVueRequest()` 处理业务并返回 JSON 响应。关闭时 `Stop()` 会同时关闭 TCP 与 HTTP 两个 acceptor。

## 已知问题与修复记录

- **构造函数参数顺序错误**(`MsgNode(int, int)`):委托构造时参数位置写反，导致 `max_len` 被传入 `-1ULL` 截断为 `-1`，触发 `max_len 必须大于 0` 错误、缓冲区未分配。已修复为 `MsgNode(-1ULL, max_len, serviceID)`。

- **客户端 io_context 线程提前退出**(`examples/Client/main.cpp` 的 `CreateConnection`):原代码先启动 `io_thread` 执行 `conn->io->run()`，但此时 `io_context` 中没有任何异步任务，`run()` 立即返回，线程随之结束；之后才调用 `Connect()`，导致 `async_resolve` 排入队列却无人驱动，连接永远不会建立，控制台无任何输出。已修复为先调用 `Connect()` 再启动 `io_thread`。

- **Windows winsock 头文件冲突**:`winsock.h` 与 `winsock2.h` 冲突导致编译报错。已在根目录 CMakeLists.txt（`common_net_options` 函数）中统一添加 `WIN32_LEAN_AND_MEAN` 与 `_WIN32_WINNT=0x0601` 编译宏，并以 `PUBLIC` 传播给所有使用者。

- **Windows 控制台 UTF-8 中文乱码**:`Utils::init()` 调用 `SetConsoleOutputCP(CP_UTF8)` 设置输出代码页，CMake 添加 `/utf-8` 编译选项保证源文件按 UTF-8 解析。

- **解析器生命周期问题**(`Client::Connect`):`async_resolve` 的 resolver 原为临时局部变量，异步解析期间对象销毁导致崩溃/未定义行为。已改为 `Client` 成员变量（`boost::asio::ip::tcp::resolver resolver;`）。

- **Windows 控制台关闭时崩溃**:原在 `ConsoleCtrlHandler` 系统回调线程中直接调用 `Stop()`（内部 `boost::asio::post`），存在竞态风险。已改为回调线程只设置 `g_exit_event` 事件唤醒主线程，由主线程统一执行优雅关闭流程。

- **关闭回调重复触发**:`ActuallyClose()` 可能被 `ReadHead` 错误、`ReadBody` 错误、`Close()` 等多次调用路径触发。已添加 `close_notified` 标记，保证 `ToClosed()` 只被调用一次。

- **关闭后继续读消息**:连接正在关闭(`closing=true`)时，`ReadHead` 解析到的消息直接丢弃并立即 `ActuallyClose()`，避免对已关闭连接继续发起异步读。

- **HTTP 会话默认不保持长连接**:`HttpSession::HttpSendResponse()` 在发送完 HTTP 响应后立即调用 `ActuallyClose()` 关闭连接。Vue3 前端每次请求都会新建 TCP 连接，适用于短请求/低频率场景；如需 keep-alive 长连接，需在 `HttpSendResponse()` 中移除末尾的 `ActuallyClose()` 并调用 `Start()` 复用解析器（注意重置 `parser_`）。

## 许可证

MIT License