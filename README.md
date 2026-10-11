# NetWorkCommunication

![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![Boost.Asio](https://img.shields.io/badge/Boost-Asio-orange)
![CMake](https://img.shields.io/badge/build-CMake-brightgreen)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

一个基于 Boost.Asio 的 C++ 网络通讯库,采用 `io_context` 单线程事件循环模型,提供异步 TCP 收发与业务逻辑解耦的回调式接口。

## 目的

- 为WebService端的通讯模块，由于是内嵌于所有的微服务之间，故分离出此服务通讯架构
- 同时为网络通讯留档
- WebService链接：[WebService](https://github.com/jyoushitou/WebService)

## 特性

- 基于 Boost.Asio 异步 I/O（`async_accept` / `async_read` / `async_write` / `async_resolve` / `async_connect`）
- 每个连接一个 `io_context`，配合 `boost::asio::post` 实现跨线程安全发送与关闭
- 消息头固定 12 字节：8 字节消息 ID + 4 字节消息长度，支持自定义业务 ID
- 跨平台字节序转换（自定义 `htonll` / `ntohll`，兼容 Windows / Linux）
- 发送队列自动串行化，`sending` 标志避免并发写 socket；`close()` 会先发完队列再关闭（优雅关闭）
- 独立的发送/接收缓冲区类（`MsgNode` / `RecvNode` / `SendNode`）
- 连接基类 `Net::Connection`（继承 `enable_shared_from_this`），封装 `readHead` / `readBody` / `doSend` / `actuallyClose`
- 服务端 `Net::Server::Server`：`start()` 同时启动监听与失效会话监控线程，`Stop()` 幂等优雅退出
- 服务端会话 `Net::Server::Session`：回调式业务处理（`HandleFunction` 返回字符串即回复内容），空闲超时自动断开（默认 60s）
- 客户端 `Net::Client::Client`：`Connect()` 异步解析并连接（`resolver` 为成员保证生命周期），连接成功后自动发送连接测试并开始读
- 客户端支持多连接并行（每连接独立 `io_context` + 独立线程）
- `Common` / `Utils` / `Net` 分层命名，构建期别名与安装后导出名一致，支持 `find_package(CommonNet)` 复用
- 强制 x64 构建兜底校验（避免与 vcpkg x64 依赖混编报 `LNK1112`）

## 环境依赖

- CMake >= 3.16
- C++17
- Boost（asio 头文件库 + `system` / `thread` 组件）
  - 安装：`vcpkg install boost-asio boost-system boost-thread`
- **Utils**：本工程的前置依赖库（日志 `Utils::Out` / 时间 `Utils::Time` / 优雅退出 `Utils::Exit`），由本机 vcpkg 的 **`utils` 端口**提供
  - 安装：`vcpkg install utils`
  - CMake 侧通过 `find_package(Utils CONFIG REQUIRED)` 引入导入目标 `Utils::Utils`
  - `Utils.h`、`Message.h`、`UtilsExport.h` 均随该包提供（**不在本仓库中**）
  - 默认三元组 `x64-windows` 下它是动态库（`Utils.dll` + 导入库 `Utils.lib`），
    构建时 vcpkg 会自动把 `Utils.dll` 复制到可执行文件旁（applocal），无需手工部署；
    若改用 `x64-windows-static` 三元组，则得到静态库并自动带上 `Utils_STATIC_DEFINE`
- vcpkg(推荐)或系统安装的 Boost（CMakeLists 自动检测 `VCPKG_ROOT`，或用 `-DCMAKE_TOOLCHAIN_FILE=...` 指定）

## 目录结构

```
NetWorkCommunication/
├── connon/                      # 公共网络库核心代码（提交）
│   ├── include/
│   │   └── NetConnection.h      # 连接基类、MsgNode/RecvNode/SendNode、协议常量声明
│   └── source/
│       └── NetConnection.cpp    # 异步收发、字节序转换、消息解析、发送队列实现
├── Server/                      # 服务端库源码（提交）
│   ├── include/
│   │   └── NetServer.h          # Session / Server 声明
│   └── source/
│       └── NetServer.cpp        # accept、会话管理、监控线程、超时清理
├── Client/                      # 客户端库源码（提交）
│   ├── include/
│   │   └── NetClient.h          # Client 声明
│   └── source/
│       └── NetClient.cpp        # 异步连接（成员 resolver）、消息回调、Stop
├── examples/                    # 示例程序入口（提交）
│   ├── CMakeLists.txt           # Server / Client 示例可执行文件定义
│   ├── Server/
│   │   └── main.cpp             # 服务端示例入口，业务回调 + Utils 优雅退出
│   └── Client/
│       └── main.cpp             # 客户端示例入口，控制台输入发送 + Utils 优雅退出
├── cmake/
│   └── CommonNetConfig.cmake.in # 安装包配置模板（find_package(CommonNet) 用）（提交）
├── lib/                         # 预留目录：本地第三方二进制/依赖，不提交（当前构建不使用）
├── build/                       # 构建产物目录（cmake -B build），不提交，已由 .gitignore 忽略
├── out/                         # IDE（VS/CMake 集成）输出目录，不提交，已由 .gitignore 忽略
├── CMakeLists.txt               # 根构建入口：统一生成 CommonUtils/CommonNetCore/CommonNetServer/CommonNetClient（提交）
├── .gitignore                   # 忽略 build/out/lib/install/*.lib/*.exe 等产物（提交）
├── LICENSE
└── README.md
```

> **提交约定**：`connon/`、`Server/`、`Client/`、`examples/`、`cmake/`、`CMakeLists.txt`、`README.md`、`LICENSE`、`.gitignore` 为源码与构建脚本，需要提交；`build/`、`out/`、`lib/`（本地第三方二进制，当前构建已不再使用）、`install/`、以及 `*.lib/*.exe/*.pdb` 等编译产物仅本地保留，不提交；Utils 依赖由 vcpkg 的 `utils` 包提供，无需入库。
>
> 注意：`Message.h`、`Utils.h` 属于 vcpkg 的 `utils` 包，**不位于 `connon/`**，工程通过 `Utils::Utils` 导入目标获取其包含目录。

## 消息协议

消息头固定 12 字节，网络字节序(Big-Endian)：

| 字段     | 长度   | 说明                                                                      |
| -------- | ------ | ------------------------------------------------------------------------- |
| 消息 ID  | 8 字节 | 业务消息标识（由调用方显式指定；发送/接收时自动做 64 位字节序转换）       |
| 消息长度 | 4 字节 | 消息体长度（不含消息头，运行时自动计算，无需手动输入）                    |
| 消息体   | 可变长 | 实际业务数据（不超过 1MB，可在 `NetConnection.h` 的 `MAX_LENGTH` 中修改） |

> 协议常量定义在 `connon/include/NetConnection.h`：`HEAD_ID_LENGTH=8`、`HEAD_LEN_LENGTH=4`、`HEAD_LENGTH=12`、`MAX_LENGTH=1MB`。

## 编译与运行

### 使用 vcpkg 安装依赖

```bash
# Utils 是本工程的前置依赖库，Boost 提供 asio 头文件与 system / thread 组件
vcpkg install utils boost-asio boost-system boost-thread
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

> 本工程默认只构建 **x64**：VS 生成器会把默认平台钉成 x64；Ninja / Makefiles 下若在 32 位命令提示符（vcvars32）中配置会被兜底校验拦下（`-DCOMMONNET_ALLOW_NON_X64=ON` 可放行），避免链接 vcpkg 的 x64 库时报 `LNK1112`。

生成的目标：

| 目标                | 类型       | 说明                                                                                        |
| ------------------- | ---------- | ------------------------------------------------------------------------------------------- |
| `CommonUtils`       | INTERFACE  | 前置依赖库 Utils 的转发目标，实际链接 vcpkg 的 `Utils::Utils`（日志 / 时间 / 优雅退出工具） |
| `CommonNetCore`     | 静态库     | 协议 / 连接核心（`connon/source/NetConnection.cpp`），链接 `CommonUtils`                    |
| `CommonNetServer`   | 静态库     | 服务端（`Server/source/NetServer.cpp`），链接 `CommonNetCore`                               |
| `CommonNetClient`   | 静态库     | 客户端（`Client/source/NetClient.cpp`），链接 `CommonNetCore`                               |
| `Server` / `Client` | 可执行文件 | 示例程序，位于构建目录 `examples/` 下（`-DCOMMONNET_BUILD_EXAMPLES=OFF` 可关闭）            |

> **命名层级**：`Common`(大功能) → `Utils` / `Net`(小功能) → `Core` / `Server` / `Client`(具体库)。
> **CMake 别名**（构建期别名与安装后导出名一致）：`Common::Utils`、`Common::Net::Core`、`Common::Net::Server`、`Common::Net::Client`。
> **依赖链**：`CommonUtils` ← `CommonNetCore` ←（`CommonNetServer` / `CommonNetClient`）。
> `CommonUtils` 本身不含实现，只是把 vcpkg 的 `Utils::Utils` 转发出去；
> 因此 `Utils.h` / `Message.h` / `UtilsExport.h`、`Utils_STATIC_DEFINE`、`Threads::Threads`
> 全部由该导入目标按构建形态自动下发，工程里不再手工维护任何 Utils 宏与库路径。

### 构建期头文件交付

除安装外，构建产物目录本身即是一个「lib + include」齐备的交付目录：`CommonNetStageHeaders` 自定义目标每次构建都会把三个公开头文件同步到 `.lib` 同级的 `include/` 下（平铺一份 + 分层 `Common/Net/...` 一份，用 `copy_if_different` 避免无谓重编）。

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

> 下游工程同样用 vcpkg 工具链配置（`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`）：
> `CommonNetConfig.cmake` 会自行 `find_dependency(Boost CONFIG COMPONENTS system thread)` 与 **`find_dependency(Utils CONFIG)`**，
> 把 vcpkg 的 Boost 与 utils 包一起找回来。Utils 的库与头文件由 vcpkg 提供，
> **不再随 CommonNet 包安装**（安装目录里只有 CommonNet 自己的三个静态库与三个公开头文件）。

### 运行

先启动服务端（示例可执行文件位于构建目录 `examples/` 下）：

```bash
# Windows
./build/examples/Debug/Server.exe
# Linux
./build/examples/Server
```

再启动客户端（启动后按提示输入服务器地址与端口，例如 `127.0.0.1 26990`）：

```bash
# Windows
./build/examples/Debug/Client.exe
# Linux
./build/examples/Client
```

## 使用示例

### 服务端

服务端的业务处理通过 `HandleFunction` 回调完成：回调在 IO 线程中执行，**返回值即作为回复内容发回客户端**。

```cpp
#include "NetServer.h"
#include "Utils.h"

boost::asio::io_context io;
boost::asio::ip::tcp::endpoint ep(boost::asio::ip::tcp::v4(), 26990);

// 业务回调：参数为（会话, 消息体），返回值作为回复内容
auto handler = std::make_shared<Net::Server::HandleFunction>(
    [](const std::shared_ptr<Net::Server::Session>& session, const std::string& msg) -> std::string
    {
        Utils::Out::outMsg("收到客户端消息: " + msg);
        // TODO: 在这里编写你的业务处理逻辑
        return "服务器已收到！";
    });

// 创建服务器（最后一个参数为会话空闲超时秒数，默认 60）
auto server = std::make_shared<Net::Server::Server>(io, ep, handler);

// 注册优雅退出回调（Ctrl+C / 关窗时由 Utils::Exit 触发）
Utils::Exit::registerStopCallback([server]() { server->Stop(); });

// 启动：内部启动失效会话监控线程并开始 accept
server->start();

// 网络线程
std::thread io_thread([&io] { io.run(); });

// 主线程阻塞等待退出信号
Utils::Exit::waitExit();

// 停止服务器（幂等，可安全重复调用）
server->Stop();
io_thread.join();
```

### 客户端

```cpp
#include "NetClient.h"
#include "Utils.h"

boost::asio::io_context io;

// 消息回调（在 IO 线程中执行，禁止长时间阻塞）
auto handler = std::make_unique<Net::Client::HandleFunction>(
    [](unsigned long long msg_id, std::string msg)
    {
        Utils::Out::outNetMsg(msg_id, "服务器发来的消息：" + msg);
    });

// 服务器地址与端口
Net::Client::HostPort HP{"127.0.0.1", "26990"};

// 创建客户端
auto client = std::make_shared<Net::Client::Client>(io, std::move(handler), HP);

// 异步连接（内部 async_resolve + async_connect，resolver 为成员保证生命周期）
// 必须在启动 IO 线程之前调用，保证 io_context 中已有异步任务
client->Connect();

// 网络线程
std::thread io_thread([&io] { io.run(); });

// 发送消息（显式指定消息 ID，线程安全，内部 post 到 IO 线程）
client->toSend(1, "hello server");

// 停止（幂等，向 IO 线程投递关闭请求，不丢弃已收到的消息）
client->Stop();

io_thread.join();
```

> `Client::HandleFunction` 为 `std::function<void(unsigned long long, std::string)>`，在构造时通过 `std::unique_ptr` 传入，**没有** `SetMessageCallback` / `SetCloseCallback` 之类的注册接口。

### 客户端多连接

`examples/Client/main.cpp` 的 `CreateConnection(HP)` 演示了标准写法：为每条连接分配独立的 `io_context`（由全局容器 `g_ios` 持有）+ 独立 `io_thread`，并**先 `Connect()` 再启动线程**（否则 `io_context` 中无异步任务，`run()` 会立即返回）：

```cpp
g_ios.push_back(std::make_unique<boost::asio::io_context>());
boost::asio::io_context& io = *g_ios.back();

ClientPtr cp;
cp.client = std::make_shared<Net::Client::Client>(
    io, std::make_unique<Net::Client::HandleFunction>(HandleWork), HP);

cp.client->Connect();          // 先发起异步连接
clients.push_back(std::move(cp));
clients.back().iot = std::thread([&io]() { io.run(); });   // 再启动 IO 线程
```

按此模式循环创建即可扩展到任意数量的并行连接（每连接一 `io_context` + 一线程）。

## 自定义业务模块指引

本库只负责「消息怎么传」，不关心「消息传什么」。业务模块复用这套通讯架构时，主要关注三件事：**服务 ID、业务处理回调、消息 ID 约定**。

### 1. 服务 ID（来自 vcpkg 的 utils 包）

服务 ID 类型为 `ServiceID`，全局变量 `Utils::serviceID` 由 `Utils.dll` 导出（`extern Utils_API std::atomic<ServiceID> serviceID;`）。
使用方只需在运行时赋值，决定日志中的服务名：

```cpp
// examples/Server/main.cpp
Utils::serviceID = RPCGateway;   // 直接赋值，禁止在消费侧再次定义该符号（否则 C2491 / LNK2005）
```

> `RPCGateway` 等枚举值定义在 vcpkg `utils` 包提供的 `Message.h` 中，不在本仓库内。

### 2. 服务端业务处理

业务逻辑写在创建 `Server` 时传入的 `HandleFunction` 回调里。回调在 IO 线程中执行，**返回的字符串即回复内容**：

```cpp
auto handler = std::make_shared<Net::Server::HandleFunction>(
    [](const std::shared_ptr<Net::Server::Session>& session, const std::string& msg) -> std::string
    {
        Utils::Out::outMsg("收到客户端消息: " + msg);

        // ===== 自定义业务逻辑开始 =====
        // 可基于 msg 内容做路由、解析 JSON、查询数据库……
        // 若需主动推送多条消息，可调用 session->reply(msg_id, data)
        // ===== 自定义业务逻辑结束 =====

        return "处理结果";   // 作为回复发回客户端
    });

auto server = std::make_shared<Net::Server::Server>(io, ep, handler);
server->start();
```

新业务模块的推荐步骤：

1. 复制整个仓库（或以子目录方式新增业务服务）；
2. 修改创建 `Server` 时的监听端口与 `ServiceID`；
3. 在 `HandleFunction` 回调中编写/扩展业务逻辑；
4. 主动推送消息用 `session->reply(msg_id, msg)`（`Session::recvToWork` 是收到消息的入口，`reply` 是回发接口）。

### 3. 客户端接入

客户端对每个业务服务建立一条连接，业务逻辑写在构造时传入的 `HandleFunction` 回调中：

```cpp
auto handler = std::make_unique<Net::Client::HandleFunction>(
    [](unsigned long long msg_id, std::string msg)
    {
        Utils::Out::outNetMsg(msg_id, "收到回复：" + msg);
    });

Net::Client::HostPort HP{"127.0.0.1", "26990"};
auto client = std::make_shared<Net::Client::Client>(io, std::move(handler), HP);
client->Connect();

// 发送带业务命令字的消息（显式指定消息 ID = 业务命令字）
client->toSend(1001, "查询订单: 20240001");
```

### 4. 消息 ID 使用约定（建议）

| 场景                       | 消息 ID 用法                                                 |
| -------------------------- | ------------------------------------------------------------ |
| 连接测试                   | 库内部连接成功后自动 `toSend(0, "check connection")`         |
| 需要命令字区分业务         | `toSend(1001, data)` 显式指定，服务端按 ID 分发              |
| 需要追踪一条消息的完整链路 | 发送端记录分配的 ID，服务端用同一 ID 回复                    |
| 自动生成唯一 ID            | 调用方自行维护计数器（示例中为 `std::atomic` 的 `g_msg_id`） |

> 本库**不会**自动分配消息 ID：`toSend` / `reply` 均要求显式传入 `msg_id`，需要自增 ID 时由业务侧自行维护。

## 设计说明

1. **单线程事件循环**：每个连接/服务器一个 `io_context`，所有 socket 读写都在其 IO 线程执行；业务线程通过 `boost::asio::post` 把发送 / 关闭任务投递到 IO 线程，避免数据竞争。

2. **跨线程发送**：`Connection::toSend()` 先做空消息 / 超长校验（非法即抛异常），`send()` 内部 `post` 到 socket executor，在 IO 线程中构建 `SendNode`（写 8 字节 ID + 4 字节长度 + 消息体）并入队。

3. **发送队列**：`sendQueue` 保存待发送消息，`sending` 标志防止并发写；发送完成后自动取出下一条继续发送。`close()` 请求后若队列仍有数据，会先发完再关闭（优雅关闭）；异步写回调中通过比对队首节点避免 `deque` 空弹出的 Debug 断言。

4. **消息缓存类**：`MsgNode` 内部维护 `char* buf`，构造时分配 `total_len + 1` 字节并将末尾置 `'\0'`，防止字符串越界；`RecvNode` / `SendNode` 继承自它。带 ID 的构造为日志追踪提供支持。

5. **跨平台字节序**：自定义 `htonll` / `ntohll` 实现 64 位网络字节序转换（Windows 无原生实现），32 位长度用 `htonl` / `ntohl`，保证协议跨平台一致。

6. **消息 ID 由调用方指定**：库不提供全局自增 ID，`toSend(msg_id, msg)` / `reply(msg_id, msg)` 均要求显式 ID；需要唯一 ID 时由业务侧维护计数器（如示例客户端的 `g_msg_id`）。

7. **优雅退出（基于 vcpkg 的 Utils）**：
   - 通过 `Utils::Exit::registerStopCallback(...)` 注册停止函数，`Utils::Exit::waitExit()` 阻塞主线程，Ctrl+C / 关窗 / 输入结束（`Utils::Exit::recviceExit()`）时统一触发。
   - 服务端：`Stop()` 置 `running=false`、`notify_all` 唤醒监控线程，并 `post` 到 IO 线程关闭 acceptor、逐个关闭会话；`~Server()` 先 `Stop()` 再 `join` 监控线程，避免 `std::thread` 析构时的 `std::terminate`。
   - 客户端：`Stop()` 向 IO 线程投递关闭请求；主线程随后 `io->stop()` 并 `join` 各 IO 线程；阻塞在 `getline` 的输入线程 detach，由进程退出统一回收。

8. **会话超时清理**：`Server::start()` 启动 `clearSessionThread`，每 60s（可被 `Stop()` 立即唤醒）向 IO 线程投递清理任务：对 `timeOut()` 且未关闭的会话发送提示并关闭，随后从 `sessions` 中移除已关闭会话。

## 已知问题与修复记录

- **Utils 前置库改为消费 vcpkg 的 `utils` 包**：原先工程把 Utils 工程编出的预编译静态库（`lib/Utils.lib` + `lib/Utilsd.lib`）用 IMPORTED 目标手工接入，并自己下发 `Utils_STATIC_DEFINE`、自己安装库与头文件——产物来源与 `/MD`、`/MDd` 的匹配全靠人工维护，Debug 配置一旦缺少对应产物就会静默退回 Release 版并报 `LNK2038`。现改为 `find_package(Utils CONFIG REQUIRED)` + `Utils::Utils`：包含目录、C++17 要求、`Threads::Threads`、以及静态形态下的 `Utils_STATIC_DEFINE` 都由导入目标自动下发，Release/Debug 两份产物（`Utils.dll` / `debug/bin/Utils.dll`）按配置自动选择。`CommonUtils` 保留为 `Common::Utils` 的转发目标，三个库与下游用法不变；安装包不再携带 Utils 的库与头文件，改由 `CommonNetConfig.cmake` 中新增的 `find_dependency(Utils CONFIG)` 找回。`Message.h` / `Utils.h` 也随该包提供，不再位于 `connon/`。

- **`Utils::serviceID` 重复定义**（`examples/Server|Client/main.cpp`）：旧版 Utils 头文件里 `serviceID` 只有 `extern` 声明、库里没有定义，示例程序只能自己补一份 `std::atomic<ServiceID> Utils::serviceID{ServiceID::Test};`。vcpkg 的 utils 包已由 `Utils.dll` 导出该符号（`extern Utils_API std::atomic<ServiceID> serviceID;`，消费侧展开为 `dllimport`），再给出定义会直接编译报错 `C2491`（绕过去也会 `LNK2005`），故两处自定义定义均已删除，改为运行时赋值 `Utils::serviceID = RPCGateway;`。

- **构造函数委托参数顺序错误**（`MsgNode`）:委托构造时参数位置写反，导致 `max_len` 被传入 `-1ULL` 截断为 `-1`，触发 `max_len 必须大于 0` 错误、缓冲区未分配。已修正为 `MsgNode(int max_len) : MsgNode(-1ULL, max_len)`。

- **客户端 io_context 线程提前退出**（`examples/Client/main.cpp` 的 `CreateConnection`）:若先启动 `io_thread` 执行 `io.run()`，此时 `io_context` 中没有任何异步任务，`run()` 立即返回，线程随之结束；之后才调用 `Connect()`，导致 `async_resolve` 排入队列却无人驱动，连接永远不会建立，控制台无任何输出。已修复为先调用 `Connect()` 再启动 `io_thread`。

- **Windows winsock 头文件冲突**:`winsock.h` 与 `winsock2.h` 冲突导致编译报错。已在根目录 CMakeLists.txt（`common_net_options` 函数）中统一添加 `WIN32_LEAN_AND_MEAN` 与 `_WIN32_WINNT=0x0601` 编译宏，并以 `PUBLIC` 传播给所有使用者。

- **Windows 控制台 UTF-8 中文乱码**:`Utils::init()` 调用 `SetConsoleOutputCP(CP_UTF8)` 设置输出代码页，CMake 添加 `/utf-8` 编译选项保证源文件按 UTF-8 解析。

- **解析器生命周期问题**(`Client::Connect`):`async_resolve` 的 resolver 原为临时局部变量，异步解析期间对象销毁导致崩溃/未定义行为。已改为 `Client` 成员变量（`boost::asio::ip::tcp::resolver resolver;`）。

- **Windows 控制台关闭时崩溃**:原在系统回调线程中直接调用 `Stop()`（内部 `boost::asio::post`），存在竞态风险。现由 `Utils::Exit` 统一：回调线程只设置退出事件唤醒主线程，由主线程安全执行 `Stop()` → `join()` 清理流程。

- **关闭回调重复触发**:`Connection::actuallyClose()` 可能被 `readHead` 错误、`readBody` 错误、`close()` 等多次路径触发。已添加 `closeNotified` 标记，保证 `toClosed()` 只被调用一次。

- **关闭后继续读消息**:连接正在关闭（`closing=true`）时，`readHead` 解析到的消息直接丢弃并立即 `actuallyClose()`，避免对已关闭连接继续发起异步读。

- **非 x64 平台静默混编**:VS 生成器 `-A Win32`、或在 32 位命令提示符中用 Ninja 配置都会悄悄编出 32 位产物，直到链接 vcpkg 的 x64 库才炸 `LNK1112`。已在根 CMakeLists.txt 于 `project()` 之前把平台钉成 x64，并在 `project()` 之后做 x64 兜底校验（`COMMONNET_ALLOW_NON_X64=ON` 可放行）。

## 许可证

MIT License
