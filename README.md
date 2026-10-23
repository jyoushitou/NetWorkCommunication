# NetWorkCommunication

![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Boost.Asio](https://img.shields.io/badge/Boost-Asio-orange.svg)
![CMake](https://img.shields.io/badge/CMake-%E2%89%A5%203.16-brightgreen.svg)
![vcpkg](https://img.shields.io/badge/deps-vcpkg-0078d4.svg)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)
![Type](https://img.shields.io/badge/library-static%20%7C%20shared-informational.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)

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
- 发送队列自动串行化，`sending` 标志避免并发写 socket；`stopToClose()` 会先发完队列再关闭（优雅关闭），`errToClose()` 则直接清空队列立即关闭
- 独立的发送/接收缓冲区类（`MsgNode` / `RecvNode` / `SendNode`）
- 连接基类 `Net::Connection`（继承 `enable_shared_from_this`），封装 `readHead` / `readBody` / `doSend` / `send` / `toSend` / `stopToClose` / `errToClose` / `offNoDelay`（内部经 `connectionClose` → `continueClose` → `socketClose` 完成关闭），`toSend` 对空消息 / 超长消息返回 `false`（不再抛异常）
- 服务端 `Net::Server::Server`：`start()` 打开监听器并启动失效会话监控线程，`stop()` 幂等优雅退出（可重复调用；`stop()` 之后还能再次 `start()` 重新监听）；`stop()` **第一次调用就真正关闭**（关 acceptor + 关会话），析构阶段也会同步收尾
- 服务端会话 `Net::Server::Session`：回调式业务处理（`HandleFunction` 签名为 `void(const std::shared_ptr<Session>&, const unsigned long long&, const std::string&)`，回调内调用 `session->reply(msg_id, msg)` 回复；`reply` 返回 `bool`，超长/空消息返回 `false`），空闲超时自动断开（默认 60s），优雅关闭超过 30s 宽限期后强制断开，收到关闭命令 `"EXIT"`（`kCloseCommand`）即关闭会话
- 服务端支持**最大并发连接数**（`maxConnections`，默认 1024，达到上限直接拒绝新连接）与**保留消息 ID**（0 = 连接测试、1 = 空闲超时提醒、2 = 忙超时提醒，业务建议从 1000 起）
- 客户端 `Net::Client::Client`：`Connect()` 异步解析并连接（`resolver` 为成员保证生命周期），连接成功后 `start()` 先发送 ID 为 `Net::kConnCheckMsgId`（0）的 `"check connection"` 测试消息再开始读；`toSend` / `stop` 均线程安全
- 客户端支持独立的关闭回调 `Net::Client::CloseFunction`（`std::function<void(const boost::system::error_code)>`），解析 / 连接失败或出错时通知业务侧，内部 `closeNotifiedByUser` 保证最多回调一次
- 客户端支持多连接并行：每连接独立 `io_context` + 独立 `io_thread`；同一个 `io_context` 只允许由一个线程 `run()`，须先 `Connect()` 再启动 IO 线程（示例中共享的 `clients` 容器由互斥量保护）
- **HTTP 服务端 `Net::Server::HttpServer`**（基于 Boost.Beast，另需 `boost-beast` / `boost-json`）：`HttpSession` 继承 TCP 的 `Session`，两段式读请求（头 → body，按 `parser->is_done()` 判断，chunked 也正确）、keep-alive 复用、一条请求只回一个响应、请求号配对防止迟到回包串到下一条请求；`httpReplay(body, status)` 同步回包、`httpAsyncReplay(seq, body, status)` 线程安全异步回包，空闲超时回 408、忙超时回 503
- `Common` / `Utils` / `Net` 分层命名，构建期别名与安装后导出名一致，支持 `find_package(CommonNet)` 复用
- 强制 x64 构建兜底校验（避免与 vcpkg x64 依赖混编报 `LNK1112`）

## 环境依赖

- CMake >= 3.16
- C++17
- Boost（asio 纯头文件库 + `system` 组件）
  - 安装：`vcpkg install boost-asio boost-system`
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
│       └── NetClient.cpp        # 异步连接（成员 resolver）、消息回调、stop
├── HttpServer/                  # HTTP 服务端库源码（提交，基于 Boost.Beast）
│   ├── include/
│   │   └── NetHttpServer.h      # HttpSession / HttpServer 声明
│   └── source/
│       └── NetHttpServer.cpp    # 请求解析、keep-alive、同步/异步回包、HTTP 巡检线程
├── examples/                    # 示例程序入口（提交）
│   ├── CMakeLists.txt           # Server / Client / HttpServer 三个示例可执行文件定义
│   ├── Server/
│   │   └── main.cpp             # TCP 服务端示例，业务回调 + Utils 优雅退出
│   ├── Client/
│   │   └── main.cpp             # TCP 客户端示例，控制台输入发送 + Utils 优雅退出
│   └── Httpserver/
│       └── main.cpp             # HTTP 服务端示例，演示同步回包与异步回包（/slow）
├── cmake/
│   └── CommonNetConfig.cmake.in # 安装包配置模板（find_package(CommonNet) 用）（提交）
├── lib/                         # 预留目录：本地第三方二进制/依赖，不提交（当前构建不使用）
├── build/                       # 构建产物目录（cmake -B build），不提交，已由 .gitignore 忽略
├── out/                         # IDE（VS/CMake 集成）输出目录，不提交，已由 .gitignore 忽略
├── CMakeLists.txt               # 根构建入口：统一生成 CommonUtils/CommonNetCore/CommonNetServer/CommonNetClient（提交）
├── .clang-format                # 统一代码格式约定（提交）
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
# Utils 是本工程的前置依赖库，Boost 提供 asio 头文件与 system 组件
vcpkg install utils boost-asio boost-system
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
| `CommonNetHttpServer` | 静态库   | HTTP 服务端（`HttpServer/source/NetHttpServer.cpp`），链接 `CommonNetServer` + `Boost::beast` / `Boost::json`（`-DCOMMONNET_BUILD_HTTP_SERVER=OFF` 可关闭） |
| `Server` / `Client` / `HttpServer` | 可执行文件 | 示例程序，位于构建目录 `examples/` 下（`-DCOMMONNET_BUILD_EXAMPLES=OFF` 可关闭） |

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
> `CommonNetConfig.cmake` 会自行 `find_dependency(Boost CONFIG COMPONENTS system)` 与 **`find_dependency(Utils CONFIG)`**，
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

HTTP 示例服务端（监听 27080，`-DCOMMONNET_BUILD_HTTP_SERVER=OFF` 时不生成）：

```bash
# Windows
./build/examples/Debug/HttpServer.exe
# Linux
./build/examples/HttpServer

# 另开终端验证
curl http://127.0.0.1:27080/          # 同步回包：{"code":0,"msg":"ok","target":"/"}
curl http://127.0.0.1:27080/slow      # 异步回包：约 500ms 后返回
```

## 使用示例

### 服务端

服务端的业务处理通过 `HandleFunction` 回调完成：回调在 IO 线程中执行，签名为 `void(const std::shared_ptr<Session>& session, const unsigned long long& msg_id, const std::string& msg)`，**回复通过回调内的 `session->reply(msg_id, msg)` 主动发出**（库不会自动回包）。

```cpp
#include "NetServer.h"
#include "Utils.h"

boost::asio::io_context io;
boost::asio::ip::tcp::endpoint ep(boost::asio::ip::tcp::v4(), 26990);

// 业务回调：参数为（会话, 消息ID, 消息体），无返回值；回复需显式调用 session->reply
auto handler = std::make_shared<Net::Server::HandleFunction>(
    [](const std::shared_ptr<Net::Server::Session>& session, const unsigned long long& msg_id,
           const std::string& msg)
        {
            Utils::Out::outMsg("收到客户端消息: " + msg);
            // TODO: 在这里编写你的业务处理逻辑
            // 用收到的同一 msg_id 回复，便于客户端追踪链路（库不会自动回包）
            session->reply(msg_id, "服务器已经收到消息");
        });

// 创建服务器：Server(io, ep, HF,
//                     timeOut=60,        // 会话空闲超时（秒）
//                     checkTime=60,      // 巡检周期（秒）
//                     maxSendBufLen=10MB,// 单会话发送队列积压上限
//                     sessionTimeOut=60, // 忙超时（供派生类使用）
//                     maxConnections=1024) // 最大并发连接数，<=0 表示不限制
auto server = std::make_shared<Net::Server::Server>(io, ep, handler, 60, 60, 1024 * 1024 * 100);

// 注册优雅退出回调（Ctrl+C / 关窗时由 Utils::Exit 触发）
// 用 weak_ptr 捕获：全局回调表没有反注册接口，按值捕获 shared_ptr 会让 Server 活过 main，
// 从而在 io_context 销毁之后才析构 acceptor（Asio 的经典 UB）
std::weak_ptr<Net::Server::Server> weakServer = server;
Utils::Exit::registerStopCallback(
    [weakServer]()
    {
        if (auto s = weakServer.lock())
        {
            s->stop();
        }
    });

// 启动：打开监听器、启动失效会话监控线程并开始 accept；端口被占用等情况会抛 std::exception
try
{
    server->start();
}
catch (const std::exception& e)
{
    Utils::Out::outErr(std::string("服务器启动失败（端口可能已被占用）：") + e.what());
    return -1;
}

// 网络线程
std::thread io_thread([&io] { io.run(); });

// 主线程阻塞等待退出信号
Utils::Exit::waitExit();

// 停止服务器（幂等；内部会关闭监听器与全部会话）
server->stop();
io_thread.join();

// 让 Server 先于 io_context 析构
server.reset();
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

// 关闭回调（连接失败/出错时被调用，最多通知一次；可为空实现）
auto closeCB = std::make_unique<Net::Client::CloseFunction>([](const boost::system::error_code) {});

// 创建客户端：Client(io, HF, HostPort, CloseFunction, maxSendBufLen)
auto client = std::make_shared<Net::Client::Client>(io, std::move(handler), HP, std::move(closeCB),
                                                   1024 * 1024 * 100);

// 异步连接（内部 async_resolve + async_connect，resolver 为成员保证生命周期）
// 必须在启动 IO 线程之前调用，保证 io_context 中已有异步任务
client->Connect();

// 网络线程
std::thread io_thread([&io] { io.run(); });

// 发送消息（显式指定消息 ID，线程安全，内部 post 到 IO 线程）
// toSend 返回 bool：false 表示消息非法（空 / 超过 MAX_LENGTH）；
// 发送队列积压超限会在 IO 线程内丢弃并记日志，不会再抛异常
if (!client->toSend(1, "hello server"))
{
    Utils::Out::outErr("消息为空或超过长度上限，未发送");
}

// 停止（幂等，向 IO 线程投递关闭请求，不丢弃已发队列中的消息）
client->stop();

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
    io, std::make_unique<Net::Client::HandleFunction>(HandleWork), HP,
    std::make_unique<Net::Client::CloseFunction>([](const boost::system::error_code) {}),
    1024 * 1024 * 100);

cp.client->Connect();          // 先发起异步连接
clients.push_back(std::move(cp));
clients.back().iot = std::thread([&io]() { io.run(); });   // 再启动 IO 线程
```

按此模式循环创建即可扩展到任意数量的并行连接（每连接一 `io_context` + 一线程）。

### HTTP 服务端

`HttpServer` 在 TCP 的 `Session` / `Server` 之上叠加 Beast 的 HTTP 解析：业务只需实现 `HttpHandleFunction`，
在回调里决定「同步回包」还是「异步回包」。

```cpp
#include "NetHttpServer.h"
#include "Utils.h"

boost::asio::io_context io;

// 基类的端点是占位用的：HttpServer 自己用 epHttp 监听，基类 acceptor 不会被打开
const boost::asio::ip::tcp::endpoint epTcp(boost::asio::ip::tcp::v4(), 0);
const boost::asio::ip::tcp::endpoint epHttp(boost::asio::ip::tcp::v4(), 27080);

auto handler = std::make_shared<Net::Server::HttpServer::HttpHandleFunction>(
    [](const std::shared_ptr<Net::Server::HttpServer::HttpSession>& session,
       const Net::Server::HttpServer::Url& url)
    {
        // url: method / target(含 query) / version / body / keep_alive / seq
        if (url.target == "/slow")
        {
            // 慢业务：不要阻塞 IO 线程，切到工作线程后用「带 seq」的异步接口回包
            auto seq = url.seq;
            std::thread([session, seq]()
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            session->httpAsyncReplay(seq, "{\"code\":0,\"msg\":\"slow ok\"}");
                        }).detach();
            return; // 回调返回时不回包，会话保持"忙"状态等这次异步回包
        }

        // 同步回包：第二个参数可指定状态码，默认 200
        session->httpReplay("{\"code\":0,\"msg\":\"ok\"}");
    });

auto server = std::make_shared<Net::Server::HttpServer::HttpServer>(io, epTcp, epHttp, handler);

try
{
    server->start(); // 打开 epHttp 监听器、启动 HTTP 巡检线程并开始 accept
}
catch (const std::exception& e)
{
    Utils::Out::outErr(std::string("HTTP 服务器启动失败（端口可能已被占用）：") + e.what());
    return -1;
}

std::thread ioThread([&io] { io.run(); });
Utils::Exit::waitExit();

server->stop(); // 关闭 httpAcceptor 与全部 HTTP 会话
ioThread.join();
```

要点：

- **一条请求只回一次包**：`httpReplay` / `httpAsyncReplay` 内部有一次性守卫，重复回包会被丢弃并记日志。
- **异步回包要带 `seq`**：`httpAsyncReplay(url.seq, body, st)` 会校验请求号，避免"迟到的回包"被当成下一条请求的响应；
  单参数版本 `httpAsyncReplay(body)` 走"当前请求"，只适合在回调内立即调用。
- **上限**：请求头默认 8KB（`maxHttpHeaderLen`）、请求体默认 10MB（`maxHttpSendBufLen`）、最大并发连接数默认 1024（`maxConnections`）。
- **自动化行为**：空闲超时回 408、业务回包超时（`httpSessionTimeOut`，默认 60s）回 503 并断开、对端正常断开静默收尾。
- `Url::body` 已由 Beast 解码（chunked 也是明文），业务无需自己处理传输编码。

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

业务逻辑写在创建 `Server` 时传入的 `HandleFunction` 回调里。回调在 IO 线程中执行，**无返回值，回复需在回调内调用 `session->reply(msg_id, msg)` 主动发出**：

```cpp
auto handler = std::make_shared<Net::Server::HandleFunction>(
    [](const std::shared_ptr<Net::Server::Session>& session, const unsigned long long& msg_id,
       const std::string& msg)
    {
        Utils::Out::outMsg("收到客户端消息: " + msg);

        // ===== 自定义业务逻辑开始 =====
        // 可基于 msg_id / msg 内容做路由、解析 JSON、查询数据库……
        // 回复（以及需要主动推送多条消息）都调用 session->reply(msg_id, data)
        // ===== 自定义业务逻辑结束 =====

        session->reply(msg_id, "处理结果");   // 作为回复发回客户端
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
auto client = std::make_shared<Net::Client::Client>(
    io, std::move(handler), HP,
    std::make_unique<Net::Client::CloseFunction>([](const boost::system::error_code) {}),
    1024 * 1024 * 100);
client->Connect();

// 发送带业务命令字的消息（显式指定消息 ID = 业务命令字）
client->toSend(1001, "查询订单: 20240001");
```

### 4. 消息 ID 使用约定（建议）

| 场景                       | 消息 ID 用法                                                 |
| -------------------------- | ------------------------------------------------------------ |
| 连接测试                   | 库内部连接成功后自动 `toSend(Net::kConnCheckMsgId /* 0 */, "check connection")` |
| 服务端超时提醒             | 库内部使用 `kServerTimeoutMsgId`(1) / `kServerBusyTimeoutMsgId`(2) |
| 需要命令字区分业务         | `toSend(1001, data)` 显式指定，服务端按 ID 分发              |
| 需要追踪一条消息的完整链路 | 发送端记录分配的 ID，服务端用同一 ID 回复                    |
| 自动生成唯一 ID            | 调用方自行维护计数器（示例中为 `std::atomic` 的 `g_msg_id`） |

保留 ID 表（定义在 `connon/include/NetConnection.h` 与 `Server/include/NetServer.h`）：

| ID | 常量 | 含义 |
| --- | --- | --- |
| 0 | `Net::kConnCheckMsgId` | 客户端连接测试 |
| 1 | `Net::Server::kServerTimeoutMsgId` | 服务端空闲超时提醒 |
| 2 | `Net::Server::kServerBusyTimeoutMsgId` | 服务端忙超时提醒 |
| ≥1000 | — | 业务消息（建议起点） |

> 本库**不会**自动分配消息 ID：`toSend` / `reply` 均要求显式传入 `msg_id`，需要自增 ID 时由业务侧自行维护。

## 设计说明

1. **单线程事件循环**：每个连接/服务器一个 `io_context`，所有 socket 读写都在其 IO 线程执行；业务线程通过 `boost::asio::post` 把发送 / 关闭任务投递到 IO 线程，避免数据竞争。

2. **跨线程发送**：`Connection::toSend()` 先做空消息 / 超长校验（非法直接返回 `false`，不抛异常），`send()` 内部 `post` 到 socket executor，在 IO 线程中构建 `SendNode`（写 8 字节 ID + 4 字节长度 + 消息体）并入队；入队前按"整帧长度"校验发送队列累计量是否超过 `maxSendBufLen`，**超限只记日志并丢弃本条消息，绝不抛异常** —— 在 IO 线程的 handler 里抛异常会冲出 `io_context::run()`，让整个 IO 线程静默退出（现场表现为"服务突然不收消息了，进程还在"）。

3. **发送队列**：`sendQueue` 保存待发送消息，`sending` 标志防止并发写；发送完成后自动取出下一条继续发送。`stopToClose()` 请求后若队列仍有数据，会先发完再关闭（优雅关闭）；异步写回调中通过比对队首节点避免 `deque` 空弹出的 Debug 断言。

4. **消息缓存类**：`MsgNode` 内部维护 `char* buf`，构造时分配 `total_len + 1` 字节并将末尾置 `'\0'`，防止字符串越界；`RecvNode` / `SendNode` 继承自它。带 ID 的构造为日志追踪提供支持。

5. **跨平台字节序**：自定义 `htonll` / `ntohll` 实现 64 位网络字节序转换（Windows 无原生实现），32 位长度用 `htonl` / `ntohl`，保证协议跨平台一致。

6. **消息 ID 由调用方指定**：库不提供全局自增 ID，`toSend(msg_id, msg)` / `reply(msg_id, msg)` 均要求显式 ID；需要唯一 ID 时由业务侧维护计数器（如示例客户端的 `g_msg_id`）。

7. **优雅退出（基于 vcpkg 的 Utils）**：
   - 通过 `Utils::Exit::registerStopCallback(...)` 注册停止函数，`Utils::Exit::waitExit()` 阻塞主线程，Ctrl+C / 关窗 / 输入结束（`Utils::Exit::recviceExit()`）时统一触发。
   - 服务端：`stop()` **无条件**置 `running=false` 并 `notify_all`，然后 `post` 到 IO 线程执行 `closeAll()`（关 acceptor + 逐个关会话 + 清空会话表）；拿不到 `selfWeak`（从未启动 / 正在析构）或 `io_context` 已停止时改为**就地同步关闭**，避免任务永远不执行导致对象与监听端口泄漏。因为 `stop()` 只依赖 `running`，所以**第一次调用就真正生效**，且可重复调用。
   - 客户端：`stop()` 向 IO 线程投递关闭请求；主线程随后 `join` 各 IO 线程（示例有意**不调用** `io->stop()`，等异步操作自然结束后 `run()` 自行返回，避免丢弃在途消息）；阻塞在 `getline` 的输入线程 detach，由进程退出统一回收。示例中共享的 `clients` 由 `g_clientsMutex` 保护，退出时**不再** `clients.clear()` / `g_ios.clear()`（detach 的输入线程可能还在访问），改为把 `g_ios` 声明在 `clients` 之前，靠静态析构的逆序保证 `io_context` 比 `Client` 活得久。

8. **会话巡检与回收**：`Server::start()` 启动 `clearSessionThread`，每 `checkTime` 秒（默认 60，可被 `stop()` 立即唤醒）向 IO 线程投递巡检任务：
   ① 对 `timeOut()` 且未关闭的会话发一条超时提醒并 `closeSession()`；
   ② 对"已请求关闭但 socket 还开着"且超过 `kCloseGraceSeconds`（30s）的会话强制 `errToClose()` —— 否则对端不读数据时 `async_write` 永远完不成，连接会被永久占着；
   ③ 回收判据用 `needReclaim()`（socket 真的关了），而不是 `isClosed()`（后者在"刚请求关闭"时就为真，会导致提前放手、谁也不再关它）。

## 已知问题与修复记录

- **Utils 前置库改为消费 vcpkg 的 `utils` 包**：原先工程把 Utils 工程编出的预编译静态库（`lib/Utils.lib` + `lib/Utilsd.lib`）用 IMPORTED 目标手工接入，并自己下发 `Utils_STATIC_DEFINE`、自己安装库与头文件——产物来源与 `/MD`、`/MDd` 的匹配全靠人工维护，Debug 配置一旦缺少对应产物就会静默退回 Release 版并报 `LNK2038`。现改为 `find_package(Utils CONFIG REQUIRED)` + `Utils::Utils`：包含目录、C++17 要求、`Threads::Threads`、以及静态形态下的 `Utils_STATIC_DEFINE` 都由导入目标自动下发，Release/Debug 两份产物（`Utils.dll`/`debug/bin/Utils.dll`）按配置自动选择。`CommonUtils`保留为`Common::Utils`的转发目标，三个库与下游用法不变；安装包不再携带 Utils 的库与头文件，改由`CommonNetConfig.cmake`中新增的`find_dependency(Utils CONFIG)` 找回。`Message.h`/`Utils.h`也随该包提供，不再位于`connon/`。

- **`Utils::serviceID` 重复定义**（`examples/Server|Client/main.cpp`）：旧版 Utils 头文件里 `serviceID` 只有 `extern` 声明、库里没有定义，示例程序只能自己补一份 `std::atomic<ServiceID> Utils::serviceID{ServiceID::Test};`。vcpkg 的 utils 包已由 `Utils.dll` 导出该符号（`extern Utils_API std::atomic<ServiceID> serviceID;`，消费侧展开为 `dllimport`），再给出定义会直接编译报错 `C2491`（绕过去也会 `LNK2005`），故两处自定义定义均已删除，改为运行时赋值 `Utils::serviceID = RPCGateway;`。

- **构造函数委托参数顺序错误**（`MsgNode`）:委托构造时参数位置写反，导致 `max_len` 被传入 `-1ULL` 截断为 `-1`，触发 `max_len 必须大于 0` 错误、缓冲区未分配。已修正为 `MsgNode(int max_len) : MsgNode(-1ULL, max_len)`。

- **客户端 io_context 线程提前退出**（`examples/Client/main.cpp` 的 `CreateConnection`）:若先启动 `io_thread` 执行 `io.run()`，此时 `io_context` 中没有任何异步任务，`run()` 立即返回，线程随之结束；之后才调用 `Connect()`，导致 `async_resolve` 排入队列却无人驱动，连接永远不会建立，控制台无任何输出。已修复为先调用 `Connect()` 再启动 `io_thread`。

- **Windows winsock 头文件冲突**:`winsock.h` 与 `winsock2.h` 冲突导致编译报错。已在根目录 CMakeLists.txt（`common_net_options` 函数）中统一添加 `WIN32_LEAN_AND_MEAN` 与 `_WIN32_WINNT=0x0601` 编译宏，并以 `PUBLIC` 传播给所有使用者。

- **Windows 控制台 UTF-8 中文乱码**:`Utils::init()` 调用 `SetConsoleOutputCP(CP_UTF8)` 设置输出代码页，CMake 添加 `/utf-8` 编译选项保证源文件按 UTF-8 解析。

- **解析器生命周期问题**(`Client::Connect`):`async_resolve` 的 resolver 原为临时局部变量，异步解析期间对象销毁导致崩溃/未定义行为。已改为 `Client` 成员变量（`boost::asio::ip::tcp::resolver resolver;`）。

- **Windows 控制台关闭时崩溃**:原在系统回调线程中直接调用 `stop()`（内部 `boost::asio::post`），存在竞态风险。现由 `Utils::Exit` 统一：回调线程只设置退出事件唤醒主线程，由主线程安全执行 `stop()` → `join()` 清理流程。
- **关闭回调重复触发**:`Connection::socketClose()` 可能被 `readHead` 错误、`readBody` 错误、`stopToClose()` / `errToClose()` 等多次路径触发。已添加 `closeNotified` 标记（`std::atomic<bool>` + `exchange`），保证派生类的 `derivedToClose()` 只被调用一次。

- **关闭后继续读消息**:连接正在关闭（`closing=true`）时，`readHead` 解析到的消息直接丢弃并调用 `stopToClose()` 走优雅关闭，避免对已关闭连接继续发起异步读（`readBody` 入口处也有同样的 `closing` 检查）。

- **`toSend` 由抛异常改为返回 `bool`**:`Connection::toSend()` 现在对空消息 / 超长消息直接返回 `false`（不再抛异常），调用方据此判断是否入队成功即可；示例客户端已改为检查返回值并记日志，不再需要 `try/catch`。

- **发送队列内存上限（`maxSendBufLen`）**:`send()` 入队前按"整帧长度"累加校验积压量（`queueBufLen`），超过 `maxSendBufLen` 时**只记日志并丢弃本条消息，不抛异常、不断连接**（抛异常会冲出 `io_context::run()`）；`queueBufLen` 在清队列 / 关闭 socket 时归零，避免计数漂移。`Server` 默认 `10MB`（非正数含负数都回退默认值），`Client` 由构造参数指定（示例给 `1024*1024*100`）。

- **非 x64 平台静默混编**:VS 生成器 `-A Win32`、或在 32 位命令提示符中用 Ninja 配置都会悄悄编出 32 位产物，直到链接 vcpkg 的 x64 库才炸 `LNK1112`。已在根 CMakeLists.txt 于 `project()` 之前把平台钉成 x64，并在 `project()` 之后做 x64 兜底校验（`COMMONNET_ALLOW_NON_X64=ON` 可放行）。

### 2026-10 修复批次（每个 bug 一条提交）

- **服务端根本没监听配置端口**：`Server` 构造函数没把 `ep` 存进成员，`openAcceptor()` 用的是默认 endpoint（端口 0），实际绑到随机端口，客户端连配置端口必然被拒。已修正初始化列表。
- **`stop()` 第一次调用空转**：`start()` 里的 `starting` 闸门在成功路径永不复位，`stop()` 一进来就命中早退分支直接 `return`，既不关监听器也不关会话（Ctrl+C 后退不出去、端口一直 LISTEN）。已删除 `starting`，`start()` 用 `running` 做幂等判据，`stop()` 无条件置位并调用 `closeAll()`。
- **重启后监听器是关的**：`stop()` 关掉 acceptor 后 `start()` 从不重开。已抽出 `openAcceptor()` / `openHttpAcceptor()`，`start()` 里按需重开。
- **监听失败的错误码丢失**：`openAcceptor(error_code ec)` 按值传参，调用方的 `ec` 永远是 success（异常消息显示 "Success"）。已改为按引用传。
- **HTTP 服务起不来**：`httpAcceptor` 从未 `open`，`start()` 判 `is_open()` 直接返回。已补回打开逻辑。
- **HTTP `stop()` 关错对象**：调的是基类 `Server::closeAll`，真正关 `httpAcceptor` / `httpSessions` 的 `closeHttpAll()` 没人调用。已改正。
- **HTTP 异步回包全部被丢弃**：`url.seq = reqSeq.fetch_add(1)` 返回旧值，与 `httpAsyncReplay` 里的 `seq != reqSeq` 恒不相等。已改为 `url.seq = ++reqSeq`。
- **HTTP 所有请求都回 500 "handler not set"**：`HttpSession` 构造函数漏了 `this->HHF = HF`，业务回调恒为空。已补上。
- **`NetClient` 编译失败**：头文件声明 `stop()`、实现写成 `Stop()`。已统一为 `stop()`。
- **优雅关闭无宽限期**：对端不读数据时 `async_write` 永不完成，会话被巡检提前移出 `sessions` 却永不关闭。已加 `closeReqTime` / `needForceClose` / `forceClose` / `needReclaim`（宽限 30s）。
- **HTTP 请求头上限恒为 1KB**：构造函数收了 `maxHttpHeaderLen` 却没赋给成员 `maxHttpHeadLen`。已补上并统一默认 8KB。
- **忙超时时间戳从未写入**：`busySince` 没有赋值点，忙会话永远不会被回收。已在置忙 / 复位处维护。
- **无最大并发连接数**：`sessions` / `httpSessions` 无上限，句柄与内存可被吃光。已加 `maxConnections`（默认 1024）。
- **`busySince` 可见性**：`HttpSession` 需要写基类的 `busySince`，原为 private（C2248）。已提升为 protected。
- **示例工程**：`examples/CMakeLists.txt` 里 `HttpServer` 目标链接错成 `Client`、`common_net_options(Httpserver)` 指向不存在的目标导致配置期失败；`examples/Httpserver/main.cpp` 是空文件；`examples/Client/main.cpp` 的 `clients` 容器缺互斥保护、`toSend` 的注释还停留在"会抛异常"。均已修正（HTTP 示例现在演示同步回包 + `/slow` 异步回包）。

## 许可证

MIT License
