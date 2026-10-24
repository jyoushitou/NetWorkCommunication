cmake_minimum_required(VERSION 3.16) # 最低 CMake 版本 3.16：INTERFACE 目标导出、GNUInstallDirs、CMakePackageConfigHelpers 在此版本均已可用

# CMake 4.x 已移除 FindBoost 模块，显式采用 NEW 行为，消除 CMP0167 回退警告
if(POLICY CMP0167) # 仅当当前 CMake 认识 CMP0167 这条策略时才设置（老版本会因未知策略直接报错）
    cmake_policy(SET CMP0167 NEW) # NEW：Boost 一律走 CONFIG 模式（用 vcpkg 的 BoostConfig.cmake），不回退到已删除的 FindBoost 模块
endif() # CMP0167 策略设置结束

# ── 依赖环境自动探测：vcpkg ─────────────────────────────
# 设计目标：只用标准的 CMake 命令就能构建，不依赖任何外部脚本或预设文件：
#     cmake -B build
#     cmake --build build --config Debug
#
# 探测顺序（命中即停，全部是只读判断，失败时给出可操作的提示）：
#   1. 调用方用 -DCMAKE_TOOLCHAIN_FILE=... 显式指定  -> 完全以调用方为准
#   2. 环境变量 VCPKG_ROOT
#   3. 构建目录里 VS/CMake 集成生成的 build.ninja 中记录的路径
#      （Visual Studio 的 CMake 集成会把 vcpkg 工具链写进该文件）
#   4. 常见安装位置（C:/vcpkg、D:/vcpkg、$ENV{HOME}/vcpkg、Program Files 等）
#   5. PATH 上的 vcpkg.exe，回溯到它的安装根
#
# 说明：第 1 步的 CMAKE_TOOLCHAIN_FILE 变量在首次 configure 后会进入缓存，
#       所以这段探测只在首次配置时真正执行。
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE) # 调用方没有显式指定工具链时才进入探测

    set(_commonnet_vcpkg_root "") # 探测结果：vcpkg 安装根

    # ① 环境变量 VCPKG_ROOT（最常见）
    if(DEFINED ENV{VCPKG_ROOT})
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _commonnet_env_root) # 反斜杠统一成正斜杠，便于后续拼接
        if(EXISTS "${_commonnet_env_root}/scripts/buildsystems/vcpkg.cmake")
            set(_commonnet_vcpkg_root "${_commonnet_env_root}")
        endif()
    endif()

    # ② 构建目录里的 build.ninja（Visual Studio 的 CMake 集成会在其中写入 vcpkg 工具链路径）
    set(_commonnet_ninja "${CMAKE_BINARY_DIR}/build.ninja")
    if(NOT _commonnet_vcpkg_root AND EXISTS "${_commonnet_ninja}")
        file(STRINGS "${_commonnet_ninja}" _commonnet_ninja_lines
            REGEX "vcpkg\\.cmake") # 只取含 vcpkg 工具链路径的行，避免整文件读入内存
        foreach(_commonnet_line IN LISTS _commonnet_ninja_lines)
            # build.ninja 里的路径带引号，取到 "/scripts/buildsystems/vcpkg.cmake" 之前的部分即安装根
            if(_commonnet_line MATCHES "([A-Za-z]:/[^\"]*)/scripts/buildsystems/vcpkg\\.cmake")
                set(_commonnet_vcpkg_root "${CMAKE_MATCH_1}") # 捕获组 1 即 vcpkg 根目录
                break()
            endif()
        endforeach()
    endif()

    # ③ 常见安装位置
    if(NOT _commonnet_vcpkg_root)
        set(_commonnet_candidates
            "C:/vcpkg"
            "D:/vcpkg"
            "E:/vcpkg"
            "$ENV{USERPROFILE}/vcpkg"
            "$ENV{HOME}/vcpkg"
            "/usr/local/vcpkg"
            "/opt/vcpkg")
        # Program Files 单独判断：该环境变量在 64 位进程里指向 "C:/Program Files"，
        # 直接拼字符串即可，无需处理带括号的变量名写法
        if(EXISTS "$ENV{ProgramFiles}/vcpkg/scripts/buildsystems/vcpkg.cmake")
            list(APPEND _commonnet_candidates "$ENV{ProgramFiles}/vcpkg")
        endif()
        foreach(_commonnet_candidate IN LISTS _commonnet_candidates)
            if(_commonnet_candidate AND EXISTS "${_commonnet_candidate}/scripts/buildsystems/vcpkg.cmake")
                set(_commonnet_vcpkg_root "${_commonnet_candidate}")
                break()
            endif()
        endforeach()
    endif()

    # ④ PATH 上的 vcpkg.exe：回溯两层即安装根（<root>/vcpkg.exe 或 <root>/x64/vcpkg.exe）
    if(NOT _commonnet_vcpkg_root)
        find_program(COMMONNET_VCPKG_EXECUTABLE NAMES vcpkg vcpkg.exe)
        if(COMMONNET_VCPKG_EXECUTABLE)
            get_filename_component(_commonnet_p1 "${COMMONNET_VCPKG_EXECUTABLE}" DIRECTORY)
            get_filename_component(_commonnet_p2 "${_commonnet_p1}" DIRECTORY)
            foreach(_commonnet_candidate IN ITEMS "${_commonnet_p1}" "${_commonnet_p2}")
                if(EXISTS "${_commonnet_candidate}/scripts/buildsystems/vcpkg.cmake")
                    set(_commonnet_vcpkg_root "${_commonnet_candidate}")
                    break()
                endif()
            endforeach()
        endif()
    endif()

    # 命中则启用；未命中不立即失败，留给下面 find_package(Utils) 决定
    # （单配置生成器如 Ninja 必须在 project() 之前设定工具链，多配置生成器可稍后再找）
    if(_commonnet_vcpkg_root)
        # 统一成绝对路径：万一 VCPKG_ROOT 给的是相对路径，写进工具链变量后
        # 会相对"构建目录"解析，换构建目录就失效，必须在写入前归一化
        get_filename_component(_commonnet_vcpkg_root "${_commonnet_vcpkg_root}" ABSOLUTE)
        set(CMAKE_TOOLCHAIN_FILE "${_commonnet_vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
            CACHE FILEPATH "vcpkg 工具链文件（由 CMakeLists.txt 自动探测得到）")
        message(STATUS "已自动探测到 vcpkg：${_commonnet_vcpkg_root}")
    else()
        message(STATUS "未探测到 vcpkg：将先在系统默认位置查找 Utils / Boost")
    endif()

endif() # 工具链探测结束

# ── 默认目标平台：固定 x64 ──────────────────────────────
# 本工程必须编成 64 位：vcpkg 只装了 x64-windows 三元组，Boost / Utils 都是 x64 产物，
# 下游（WebServer 等）同样是 x64。一旦编成 Win32(x86)，链接期会撞上
# LNK1112「模块计算机类型与目标计算机类型冲突」，而报错现场离原因很远，排查成本高。
#
# 所以在第一次 project() 之前把平台钉成 x64（此变量必须早于 project() 才生效）：
#   - Visual Studio 生成器：等价于命令行 -A x64。不写这一句时默认平台随 CMake 版本走，
#     老版本（本项目声明的最低 3.16）默认是 Win32，换机器 / 换 CMake 就可能悄悄变成 32 位。
#   - Ninja / Makefiles：该变量对它们无效，架构由启动 shell 的环境决定，
#     因此另有 project() 之后的兜底校验（见下）。
# 只在调用方没有显式指定平台时才填默认值：传了 -A / -DCMAKE_GENERATOR_PLATFORM=... 就以调用方为准，
# 非 x64 的情形再由下面的架构校验收口。
if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND NOT CMAKE_GENERATOR_PLATFORM) # 仅 VS 生成器、且调用方没有用 -A 显式指定平台时才填默认值
    set(CMAKE_GENERATOR_PLATFORM x64) # 必须是「普通变量」：命令行 -A 会把同名 cache 条目预先建成 INTERNAL，此时 set(... CACHE STRING ...) 是空操作（已实测无效），普通变量才能真正改变生成器平台
endif() # 平台默认值设置结束

project(NetWorkCommunication # 声明工程：确定 PROJECT_NAME / PROJECT_VERSION，并触发编译器探测
    VERSION 1.0.0 # 版本号：会写进 CommonNetConfigVersion.cmake，供下游 find_package(CommonNet 1.0) 做兼容判断
    DESCRIPTION "Boost.Asio based C++ network communication library" # 工程描述，仅作文档用途
    LANGUAGES CXX) # 只启用 C++：纯 C++ 工程，不必让 CMake 额外探测 C 编译器

# ── 目标架构兜底校验（默认只放行 x64）────────────────────
# 两条路径都可能悄悄编出非 x64 产物，而 CMake 全程不报错：
#   VS 生成器        ：-A Win32 / -A ARM64
#   Ninja / Makefiles：架构完全由启动 shell 的环境决定 ——
#                      "Developer Command Prompt"（vcvars32）-> x86 ✗
#                      "x64 Native Tools Command Prompt"（vcvars64）-> x64 ✓
# 直到链接 vcpkg 的 x64 库时才炸 LNK1112，现场离原因很远。
# 故在 project() 探测完平台后立即校验，把问题挡在编译之前。
option(COMMONNET_ALLOW_NON_X64 "允许非 x64 架构（x86 / ARM64）构建；默认关闭，仅当依赖也换成对应架构时才需要打开" OFF) # 逃生开关：确实要别的架构时用 -DCOMMONNET_ALLOW_NON_X64=ON
if(NOT COMMONNET_ALLOW_NON_X64) # 默认只放行 x64
    if(CMAKE_VS_PLATFORM_NAME AND NOT CMAKE_VS_PLATFORM_NAME STREQUAL "x64") # VS 生成器：平台名直接反映 -A 的结果（空表示不是 VS 生成器）
        message(FATAL_ERROR "检测到非 x64 平台（CMAKE_VS_PLATFORM_NAME=${CMAKE_VS_PLATFORM_NAME}），但本工程默认只构建 x64。" # 立刻失败，别等链接期
            "\n  * 重新配置时不传 -A（本工程默认 x64），或显式 -A x64"
            "\n  * 若该构建目录是旧的非 x64 缓存：删掉整个目录后重新配置（生成器平台一旦写进缓存就无法原地改）"
            "\n  * 确实需要该架构：加 -DCOMMONNET_ALLOW_NON_X64=ON（注意 vcpkg 需另装对应架构的三元组）")
    elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 8) # Ninja / Makefiles：无法感知平台名，只能按指针宽度判断（4 字节 = 32 位）
        message(FATAL_ERROR "检测到 32 位目标（CMAKE_SIZEOF_VOID_P=${CMAKE_SIZEOF_VOID_P}），但本工程默认只构建 x64。" # 立刻失败，别等链接期
            "\n  * Ninja / Makefiles    ：改用 \"x64 Native Tools Command Prompt for VS\"（vcvars64），不要用 32 位的命令提示符"
            "\n  * Visual Studio 生成器：不传 -A（本工程默认 x64），或显式 -A x64"
            "\n  * 确实需要 32 位       ：加 -DCOMMONNET_ALLOW_NON_X64=ON（注意 vcpkg 需另装 x86-windows 三元组）")
    endif() # 架构判断结束
endif() # 架构校验结束

# ── 构建选项 ────────────────────────────────────────────
option(COMMONNET_BUILD_EXAMPLES "构建 Server/Client 示例可执行文件" ON) # 默认编译 examples 下的示例，可用 -DCOMMONNET_BUILD_EXAMPLES=OFF 关闭
option(COMMONNET_BUILD_HTTP_SERVER "构建 HTTP 服务端库 CommonNetHttpServer（在 Server 之上叠加，另需 boost-beast / boost-json）" ON) # 默认构建；该库的外部依赖比其它三个库多，确实不想要时用 -DCOMMONNET_BUILD_HTTP_SERVER=OFF 关掉
# 库类型由 CMake 内置的 BUILD_SHARED_LIBS 控制（默认未定义 = 静态库），
# 无需在此声明：用 -DBUILD_SHARED_LIBS=ON 即可切为动态库。

set(CMAKE_CXX_STANDARD 17) # 全工程统一 C++17（用到了 std::atomic、if constexpr 等）
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 编译器不支持 C++17 时直接报错，而不是悄悄降级
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES) # 仅单配置生成器、且调用方未指定时
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型") # 兜底 Release；不加 FORCE，调用方显式指定优先
endif()

# 生成 compile_commands.json 供 clangd / C/C++ 扩展取得精确的 -I / -D，
# 从而无需维护 IDE 专用配置文件。该变量只对 Ninja / Makefile 生成器有效，
# 多配置生成器（Visual Studio）下设置它会被 CMake 忽略并给出告警，故加条件。
if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# 安装目录变量（CMAKE_INSTALL_INCLUDEDIR / CMAKE_INSTALL_LIBDIR）在本文件前段
# 就会写进目标的 INSTALL_INTERFACE，故在定义目标之前先引入
include(GNUInstallDirs) # 引入 GNU 标准安装目录变量：include / lib / bin（Windows 上同样适用，且可被 -DCMAKE_INSTALL_LIBDIR=... 覆盖）

# ── 查找依赖库：Utils（前置库，vcpkg 的 utils 端口）与 Boost ──────
#   Utils —— 导入目标 Utils::Utils，提供 Utils.h / Message.h / UtilsExport.h
#   Boost —— 提供 asio（纯头文件）与 system
#
# 两级查找：先按调用方给定的路径找；找不到再尝试从 vcpkg 安装目录补一条搜索路径。
# 这样"VCPKG_ROOT 指向别处"或"调用方自己指定了工具链"的情况都能兼容。
find_package(Utils CONFIG QUIET)

if(NOT Utils_FOUND)
    # 从工具链路径回溯 vcpkg 安装根（工具链形如 <root>/scripts/buildsystems/vcpkg.cmake），
    # 补一条 installed/<triplet> 搜索路径后重试。
    # 注意：这里必须显式给出三元组作为兜底 —— 不能依赖 vcpkg 工具链里已经算好的
    # VCPKG_TARGET_TRIPLET，因为工具链在 project() 中才运行，此刻它可能尚未生效。
    if(CMAKE_TOOLCHAIN_FILE MATCHES "^(.*)/scripts/buildsystems/vcpkg\\.cmake$")
        set(_commonnet_vcpkg_from_tc "${CMAKE_MATCH_1}")
        if(NOT VCPKG_TARGET_TRIPLET)
            set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg 三元组（仅用于依赖搜索兜底）")
        endif()
        list(APPEND CMAKE_PREFIX_PATH "${_commonnet_vcpkg_from_tc}/installed/${VCPKG_TARGET_TRIPLET}")
        find_package(Utils CONFIG QUIET)
    endif()
endif()

if(NOT Utils_FOUND)
    message(FATAL_ERROR
        "未找到 Utils 前置库（vcpkg 的 utils 端口，提供 Utils.h / Message.h / UtilsExport.h）。\n"
        "请任选一种方式解决：\n"
        "  1. 安装依赖（推荐）：\n"
        "         vcpkg install utils:x64-windows boost-asio:x64-windows boost-system:x64-windows\n"
        "  2. 指定 vcpkg 位置（三种等价写法）：\n"
        "         cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake\n"
        "         set VCPKG_ROOT=<vcpkg>  然后重新配置\n"
        "         cmake -B build -DCMAKE_PREFIX_PATH=<vcpkg>/installed/x64-windows\n"
        "  3. Utils 装在别处：cmake -B build -DCMAKE_PREFIX_PATH=<Utils 安装前缀>")
endif()

# Boost：只需要 system。asio 是纯头文件，随 boost-headers 提供，无需组件名。
# 注意：不要声明 project 里没用到的组件（例如 thread）—— vcpkg 会因此拉入多余依赖。
find_package(Boost CONFIG REQUIRED COMPONENTS system)

# ── 公共属性封装（避免三个库重复书写）────────────────────
# 所有公开头文件都通过 asio，故 Windows 宏需以 PUBLIC 传播给使用者
function(common_net_options target) # 定义辅助函数：给目标统一加语言标准、Windows 宏与 MSVC 编译选项；形参 target 为目标名
    target_compile_features(${target} PUBLIC cxx_std_17) # 要求 C++17，并以 PUBLIC 传播：使用者无需自己再设 CMAKE_CXX_STANDARD
    if(WIN32) # 仅 Windows 需要的宏
        target_compile_definitions(${target} PUBLIC WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601) # PUBLIC：不下发会让使用者踩 winsock.h/winsock2.h 冲突；_WIN32_WINNT 保证 Vista+ 的 API 声明可见
    endif() # Windows 分支结束
    if(MSVC) # 仅 MSVC 认识的编译选项（GCC/Clang 传了会报错）
        target_compile_options(${target} PRIVATE /MP /FS) # PRIVATE：只影响本库自身编译 —— /MP 多进程编译、/FS 串行化 PDB 写入
        # /utf-8 必须 PUBLIC 下发给使用者：公开头文件含中文注释，且是 UTF-8（无 BOM）+ LF 换行。
        # 使用者若按系统 ANSI（中文 Windows 是 GBK）解析，注释末尾的多字节字符会和换行组合，
        # 把下一行代码并进注释，随后就是一片语法错误 —— 实测 vcpkg 装出来的包被下游 #include 时
        # 报 NetServer.h(49)「必须初始化引用」，而 HandleFunction 实际在 56 行，正好差 7 行。
        # 用生成器表达式限定只在 MSVC 下发，避免把 /utf-8 塞给 GCC/Clang 使用者。
        target_compile_options(${target} PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    endif() # MSVC 分支结束
endfunction() # common_net_options 定义结束

# ── 工具库：Utils（前置依赖库，消费 vcpkg 的 utils 包）──────────
# Utils 是本工程的前置库，由 vcpkg 的 utils 端口提供，导入目标 Utils::Utils。
# 该导入目标自带全部必要信息，本工程无需手工下发任何宏或包含目录：
#   - INTERFACE_INCLUDE_DIRECTORIES ：Utils.h / Message.h / UtilsExport.h
#   - INTERFACE_COMPILE_FEATURES    ：cxx_std_17
#   - INTERFACE_LINK_LIBRARIES      ：Threads::Threads
#   - 静态三元组（x64-windows-static）下另带 Utils_STATIC_DEFINE
#
# 默认三元组 x64-windows 下 Utils::Utils 是**动态库**（bin/Utils.dll），
# Release / Debug 两份产物由导入目标按当前配置自动选择，因此既不会出现
# 「VS 多配置生成器下 Debug 静默退回 Release」，也不必操心 RuntimeLibrary 与
# _ITERATOR_DEBUG_LEVEL 是否一致（原 LNK2038 风险消失）。
# 正因如此，动态库场景**不能定义 Utils_STATIC_DEFINE**：那会让 Utils_API 展开为空，
# Utils::serviceID 这类导出数据退化成普通 extern（链接期 LNK2019）。
#
# 对外名字保持 Common::Utils 不变：它现在只是一个把 Utils::Utils 转发出去的
# INTERFACE 目标，三个库的链接行与下游用法都无需改动。
add_library(CommonUtils INTERFACE) # INTERFACE 库：只承载「接口」（包含目录 / 编译宏 / 链接依赖），本身不产出 .lib
target_link_libraries(CommonUtils INTERFACE Utils::Utils) # 把 Utils::Utils 原样转发出去：包含目录、cxx_std_17、Threads::Threads、静态形态的 Utils_STATIC_DEFINE 全部随之传递
add_library(Common::Utils ALIAS CommonUtils) # 构建期别名，与安装后的导出名保持一致
set_target_properties(CommonUtils PROPERTIES EXPORT_NAME Utils) # 导出名 Utils：配合 install(EXPORT) 的 NAMESPACE Common::，下游看到的就是 Common::Utils

# ── 核心库：协议 / 连接 ─────────────────────────────────
add_library(CommonNetCore # 核心库：协议与连接（默认静态库，随 -DBUILD_SHARED_LIBS=ON 可变为动态库）
    connon/source/NetConnection.cpp) # 唯一实现文件：异步收发、字节序转换、消息解析、发送队列
common_net_options(CommonNetCore) # 套用公共属性（C++17 / Windows 宏 / MSVC /MP /FS /utf-8）
target_include_directories(CommonNetCore PUBLIC # PUBLIC：自己编译要用，使用者也要（NetConnection.h 是公开头文件）
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/connon/include> # 构建期：直接用源码树里的 connon/include，改头文件即时生效
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Core>) # 安装后：<prefix>/include/Common/Net/Core
target_link_libraries(CommonNetCore PUBLIC CommonUtils Boost::system) # PUBLIC：依赖会随 Common::Net::Core 一起传递给下游
add_library(Common::Net::Core ALIAS CommonNetCore) # 构建期别名
# 导出名对齐构建期别名，使用者统一用 Common::Net::Core
set_target_properties(CommonNetCore PROPERTIES EXPORT_NAME Net::Core) # 导出名 Net::Core → 下游即 Common::Net::Core

# ── 服务端库 ────────────────────────────────────────────
add_library(CommonNetServer # 服务端库（默认静态库）
    Server/source/NetServer.cpp) # 实现文件：accept、会话管理、会话超时巡检与回收
common_net_options(CommonNetServer) # 套用公共属性
target_include_directories(CommonNetServer PUBLIC # PUBLIC：NetServer.h 是公开头文件
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Server/include> # 构建期包含目录
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Server>) # 安装后包含目录
target_link_libraries(CommonNetServer PUBLIC CommonNetCore) # 依赖核心库；PUBLIC：下游链接 Server 时自动带上 Core 及其全部依赖
add_library(Common::Net::Server ALIAS CommonNetServer) # 构建期别名
set_target_properties(CommonNetServer PROPERTIES EXPORT_NAME Net::Server) # 导出名 Net::Server → 下游即 Common::Net::Server

# ── 客户端库 ────────────────────────────────────────────
add_library(CommonNetClient # 客户端库（默认静态库）
    Client/source/NetClient.cpp) # 实现文件：异步连接（成员 resolver）、消息回调、关闭回调
common_net_options(CommonNetClient) # 套用公共属性
target_include_directories(CommonNetClient PUBLIC # PUBLIC：NetClient.h 是公开头文件
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Client/include> # 构建期包含目录
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Client>) # 安装后包含目录
target_link_libraries(CommonNetClient PUBLIC CommonNetCore) # 依赖核心库（PUBLIC，语义同上）
add_library(Common::Net::Client ALIAS CommonNetClient) # 构建期别名
set_target_properties(CommonNetClient PROPERTIES EXPORT_NAME Net::Client) # 导出名 Net::Client → 下游即 Common::Net::Client

# ── HTTP 服务端库（依赖服务端库 Server）────────────────────
# HttpServer 是在 Server 之上叠加的一层 HTTP 服务端，本库只负责通讯：
#   Net::Server::HttpServer::HttpSession 继承 Net::Server::Session
#   Net::Server::HttpServer::HttpServer  继承 Net::Server::Server
# 所以它唯一的库依赖就是 CommonNetServer —— Server 已经 PUBLIC 依赖 Core，Core 又
# PUBLIC 依赖 Utils，TCP 侧的头文件（NetServer.h / NetConnection.h）与库
# （CommonNetServer / CommonNetCore / Utils）都会顺着这条链接行自动传递过来，无需重复书写。
#
# 除 Server 之外只剩 Boost 一项外部依赖，故用 COMMONNET_BUILD_HTTP_SERVER 单独开关：
#   boost-beast / boost-json ：HTTP 报文解析与 JSON 解析，属通讯层自身的协议处理。
#                              两者都是纯头文件库，不会引入链接期依赖，但 vcpkg 必须装了对应端口才有头文件。
#
# 本层不做业务序列化，所以不依赖 protobuf：请求体的业务语义（编解码、服务号、命令号等）
# 一律留给业务层，这一层只负责把 HTTP 报文收发清楚。
if(COMMONNET_BUILD_HTTP_SERVER) # 开关打开时才定义本库（开关见文件上方的 option 声明）

    # 补齐 Boost 组件：文件上方已按 components system 找过一次，这里追加 beast / json。
    # 不加 REQUIRED 而是 QUIET + 自检，是为了在缺组件时给出"装哪个 vcpkg 端口"的可操作提示，
    # 而不是只抛 CMake 自带的 "Could not find a package configuration file"。
    find_package(Boost CONFIG QUIET COMPONENTS beast json)
    if(NOT Boost_FOUND)
        message(FATAL_ERROR
            "未找到 Boost 的 beast / json 组件（HTTP 服务端库需要）。\n"
            "请安装依赖：\n"
            "    vcpkg install boost-beast:x64-windows boost-json:x64-windows\n"
            "或临时跳过该库：cmake -B build -DCOMMONNET_BUILD_HTTP_SERVER=OFF")
    endif()

    add_library(CommonNetHttpServer # HTTP 服务端库（默认静态库）
        HttpServer/source/NetHttpServer.cpp) # 实现文件：HTTP 会话（HttpSession）与 HTTP acceptor（HttpServer）
    common_net_options(CommonNetHttpServer) # 套用公共属性（C++17 / Windows 宏 / MSVC /MP /FS /utf-8）
    target_include_directories(CommonNetHttpServer PUBLIC # PUBLIC：NetHttpServer.h 是公开头文件
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/HttpServer/include> # 构建期：源码树里的 HttpServer/include
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/HttpServer>) # 安装后：<prefix>/include/Common/Net/HttpServer
    target_link_libraries(CommonNetHttpServer PUBLIC # PUBLIC：下面这些依赖全部出现在 NetHttpServer.h 的公开接口上，会随本库传递给下游
        CommonNetServer # 依赖服务端库（本题要求）：HttpSession/HttpServer 分别继承 Server 的 Session/Server
        Boost::beast Boost::json) # HTTP 报文与 JSON：纯头文件导入目标，只为下发包含目录
    add_library(Common::Net::HttpServer ALIAS CommonNetHttpServer) # 构建期别名
    set_target_properties(CommonNetHttpServer PROPERTIES EXPORT_NAME Net::HttpServer) # 导出名 Net::HttpServer → 下游即 Common::Net::HttpServer

    # 供 CommonNetConfig.cmake.in 在生成期判断：下游包是否需要连带 find_dependency 上面这项额外依赖
    set(COMMONNET_HTTP_SERVER_ENABLED TRUE)

else() # 开关关闭：不定义该库，也不把它写进导出集与安装规则

    set(COMMONNET_HTTP_SERVER_ENABLED FALSE) # 生成的 CommonNetConfig.cmake 不再查找 beast / json

endif() # HTTP 服务端库开关结束

# ── 构建期头文件交付：把公开头文件同步到 .lib 同级目录 ──────────
# 让构建产物目录本身就是一个「lib + include」齐备、可直接被其他工程引用的交付目录，
# 不必先 install 再取用；install 规则保持不变（两者互不影响）。
#
# 为什么用 $<TARGET_FILE_DIR:...> 而不是写死 ${CMAKE_BINARY_DIR}：
#   .lib 的落地目录随生成器而变（本机实测）：
#       Ninja / Makefiles（单配置）  ->  <构建目录>/              例如 out/build/x64-Debug/
#       Visual Studio（多配置）      ->  <构建目录>/<配置>/        例如 build/Debug/
#   该生成器表达式在两种生成器下都指向 .lib 的真实目录，
#   于是 include/ 始终与 .lib 同级，换生成器不必改配置。
#
# 为什么两种目录结构各放一份：
#   include/NetServer.h                    -> 下游只加一个 -I<lib>/include 即可（各头文件名互不冲突）
#   include/Common/Net/Server/NetServer.h  -> 与 install 后的 INSTALL_INTERFACE 布局完全一致
#
# 为什么挂在 ALL 自定义目标上、而不是库的 POST_BUILD：
#   POST_BUILD 只在库被重新链接时才触发；而只改头文件（注释、默认参数）时库未必重新链接，
#   副本就会悄悄过期 —— 这正是先前 IntelliSense 解析到旧 NetConnection.h 的成因。
#   这里每次构建都参与同步，配合 copy_if_different 只在内容真正变化时写盘，
#   既不漏同步，也不会因时间戳刷新导致下游无谓重编。
#
# 注意：Utils.h / Message.h / UtilsExport.h 属于 vcpkg 的 utils 包，不在此复制，
#       其包含目录仍由 Utils::Utils 导入目标提供。
set(COMMONNET_PUBLIC_HEADERS # 公开头文件清单：<源文件相对路径>|<安装用子目录>（用 | 分隔，避开 CMake 列表的 ; 语义）
    "connon/include/NetConnection.h|Common/Net/Core" # 核心库头文件
    "Server/include/NetServer.h|Common/Net/Server" # 服务端库头文件
    "Client/include/NetClient.h|Common/Net/Client") # 客户端库头文件

if(COMMONNET_BUILD_HTTP_SERVER) # HTTP 服务端库开启时，它的公开头文件同样要参与同步
    list(APPEND COMMONNET_PUBLIC_HEADERS
        "HttpServer/include/NetHttpServer.h|Common/Net/HttpServer") # HTTP 服务端库头文件
endif() # 清单追加结束

set(COMMONNET_STAGE_INCLUDEDIR "$<TARGET_FILE_DIR:CommonNetCore>/include") # 交付目录：与 .lib 同级
set(COMMONNET_STAGE_COMMANDS) # 累积同步命令（下面按头文件逐条追加）
foreach(header_entry IN LISTS COMMONNET_PUBLIC_HEADERS) # 遍历全部公开头文件
    string(REPLACE "|" ";" header_fields "${header_entry}") # 拆成「源路径 / 子目录」两段
    list(GET header_fields 0 header_rel) # 形如 connon/include/NetConnection.h
    list(GET header_fields 1 header_subdir) # 形如 Common/Net/Core
    get_filename_component(header_name "${header_rel}" NAME) # 取文件名，形如 NetConnection.h
    list(APPEND COMMONNET_STAGE_COMMANDS # 追加该头文件的同步命令：建分层子目录 + 写两份副本
        COMMAND ${CMAKE_COMMAND} -E make_directory "${COMMONNET_STAGE_INCLUDEDIR}/${header_subdir}" # 平铺那份落在 include/ 根（复制时自动创建），这里补的是分层子目录
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${header_rel}" "${COMMONNET_STAGE_INCLUDEDIR}/${header_name}" # 平铺副本：include/<文件名>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${header_rel}" "${COMMONNET_STAGE_INCLUDEDIR}/${header_subdir}/${header_name}") # 分层副本：include/<子目录>/<文件名>
endforeach() # 头文件清单遍历结束

add_custom_target(CommonNetStageHeaders ALL # ALL：纳入默认构建，每次构建做一次同步检查
    ${COMMONNET_STAGE_COMMANDS} # 上面累积的复制命令
    COMMENT "同步公开头文件到 .lib 同级的 include/（平铺 + 分层各一份）" # 构建时打印的一行说明
    VERBATIM) # VERBATIM：参数原样传递，避免路径中的空格/分号被额外解释

# 显式声明依赖：头文件同步必须在所有库构建完成之后进行。
# 目标命令里虽然用到了 $<TARGET_FILE_DIR:CommonNetCore>，但那只是"取值"，
# 不会建立构建顺序依赖；并行构建（cmake --build build --parallel）时若无此行，
# 同步与库编译可能交错，产物目录会短暂出现"新头文件 + 旧库"的组合。
add_dependencies(CommonNetStageHeaders CommonNetCore CommonNetServer CommonNetClient)
if(COMMONNET_BUILD_HTTP_SERVER) # HTTP 服务端库同样纳入这套顺序约束
    add_dependencies(CommonNetStageHeaders CommonNetHttpServer)
endif() # HTTP 服务端库依赖声明结束

# ── 示例可执行文件 ──────────────────────────────────────
if(COMMONNET_BUILD_EXAMPLES) # 开关打开时才进入子目录
    add_subdirectory(examples) # 处理 examples/CMakeLists.txt：生成 Server / Client 两个示例可执行文件
endif() # 示例开关结束

# ── 安装与导出 ──────────────────────────────────────────
include(CMakePackageConfigHelpers) # 引入 configure_package_config_file / write_basic_package_version_file 两个辅助函数

# CommonUtils 是 INTERFACE 目标（本身没有产物），列在这里只是为了把它写进导出集，
# 让下游仍能用 Common::Utils 这个名字；Utils 的库与头文件已由 vcpkg 管理，
# 不再随本包安装——CommonNetConfig.cmake 会 find_dependency(Utils) 找回它们。
install(TARGETS CommonUtils CommonNetCore CommonNetServer CommonNetClient # 安装这四个目标，并登记进导出集 CommonNetTargets
    EXPORT CommonNetTargets # 导出集名：install(EXPORT) 据此生成 CommonNetTargets.cmake
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} # 运行期产物（Windows 下的 .dll）→ bin/
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # 共享库（Linux 的 .so）→ lib/
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}) # 静态库与导入库（.lib / .a）→ lib/

if(COMMONNET_BUILD_HTTP_SERVER) # HTTP 服务端库：登记进同一个导出集（install(TARGETS) 可按目标分多次调用，导出集是累加的）
    install(TARGETS CommonNetHttpServer
        EXPORT CommonNetTargets # 与上面四个库同一个导出集，下游看到的仍是 Common::Net::HttpServer
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} # 运行期产物（Windows 下的 .dll）→ bin/
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # 共享库（Linux 的 .so）→ lib/
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}) # 静态库与导入库（.lib / .a）→ lib/
endif() # HTTP 服务端库安装结束

# 核心头文件：CommonNetCore 的接口会带上 Common::Utils（即 Utils::Utils），
# 其包含目录由 vcpkg 的导入目标提供，故 #include "Message.h" 依旧能找到
install(FILES # 安装核心头文件
    connon/include/NetConnection.h # 供下游 #include "NetConnection.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Core) # 落到 include/Common/Net/Core，与上面的 INSTALL_INTERFACE 一致

install(FILES # 安装服务端头文件
    Server/include/NetServer.h # 供下游 #include "NetServer.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Server) # 落到 include/Common/Net/Server

install(FILES # 安装客户端头文件
    Client/include/NetClient.h # 供下游 #include "NetClient.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Client) # 落到 include/Common/Net/Client

if(COMMONNET_BUILD_HTTP_SERVER) # HTTP 服务端库头文件
    install(FILES # 安装 HTTP 服务端头文件
        HttpServer/include/NetHttpServer.h # 供下游 #include "NetHttpServer.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/HttpServer) # 落到 include/Common/Net/HttpServer
endif() # HTTP 服务端库头文件安装结束

install(EXPORT CommonNetTargets # 把导出集写成下游可 include 的 targets 文件
    FILE CommonNetTargets.cmake # 生成文件名
    NAMESPACE Common:: # 统一加前缀：CommonUtils→Common::Utils、CommonNetCore→Common::Net::Core、Server/Client 同理
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CommonNet) # 与 CommonNetConfig.cmake 同目录，便于 find_package 一并找到

configure_package_config_file( # 由模板生成 CommonNetConfig.cmake（替换 @PACKAGE_INIT@ 等占位符）
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CommonNetConfig.cmake.in # 模板：负责 find_dependency 与 include targets 文件
    ${CMAKE_CURRENT_BINARY_DIR}/CommonNetConfig.cmake # 生成到构建目录，随后由 install(FILES) 落盘
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CommonNet) # 告知安装位置，模板据此算出 PACKAGE_PREFIX_DIR

write_basic_package_version_file( # 生成版本文件，供下游 find_package(CommonNet 1.0) 之类做兼容性判断
    ${CMAKE_CURRENT_BINARY_DIR}/CommonNetConfigVersion.cmake # 生成路径
    VERSION ${PROJECT_VERSION} # 版本号取自 project(... VERSION)，即 1.0.0
    COMPATIBILITY SameMajorVersion) # 兼容策略：主版本相同即算兼容（1.x 可满足对 1.y 的请求）

install(FILES # 安装上面生成的两个包配置文件
    ${CMAKE_CURRENT_BINARY_DIR}/CommonNetConfig.cmake # 包配置：find_package(CommonNet) 的入口
    ${CMAKE_CURRENT_BINARY_DIR}/CommonNetConfigVersion.cmake # 版本文件：配合 find_package 的版本要求
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CommonNet) # 落到 lib/cmake/CommonNet
