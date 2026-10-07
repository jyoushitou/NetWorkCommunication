cmake_minimum_required(VERSION 3.16) # 最低 CMake 版本 3.16：INTERFACE 目标导出、GNUInstallDirs、CMakePackageConfigHelpers 在此版本均已可用

# CMake 4.x 已移除 FindBoost 模块，显式采用 NEW 行为，消除 CMP0167 回退警告
if(POLICY CMP0167) # 仅当当前 CMake 认识 CMP0167 这条策略时才设置（老版本会因未知策略直接报错）
    cmake_policy(SET CMP0167 NEW) # NEW：Boost 一律走 CONFIG 模式（用 vcpkg 的 BoostConfig.cmake），不回退到已删除的 FindBoost 模块
endif() # CMP0167 策略设置结束

# ── 通过 vcpkg 查找依赖（可移植写法，沿用原有子工程逻辑）─────────
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE) # 调用方没有用 -DCMAKE_TOOLCHAIN_FILE=... 显式指定工具链时，才自动探测
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake") # 环境变量 VCPKG_ROOT 已设置，且其下确实存在 vcpkg 工具链文件
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" # 采用环境里的 vcpkg 工具链（它负责把 installed/<triplet> 加进搜索路径）
            CACHE STRING "vcpkg toolchain file") # 写进缓存，后续 configure 复用；可在 CMakeCache.txt 中查看与覆盖
    else() # 环境变量缺失或指向的目录里没有工具链
        message(FATAL_ERROR "未找到 vcpkg 工具链：请设置环境变量 VCPKG_ROOT，或使用 -DCMAKE_TOOLCHAIN_FILE=... 指定 vcpkg.cmake") # 立即失败：没有工具链后面找 Boost / Utils 必然失败，早报错比晚报错清楚
    endif() # 环境变量探测分支结束
endif() # 工具链探测结束

set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "使用 vcpkg classic 已安装依赖" FORCE) # 关闭 manifest 模式：本项目用 vcpkg classic（vcpkg install 装到全局 installed/<triplet>），不引入 vcpkg.json

project(NetWorkCommunication # 声明工程：确定 PROJECT_NAME / PROJECT_VERSION，并触发编译器探测
    VERSION 1.0.0 # 版本号：会写进 CommonNetConfigVersion.cmake，供下游 find_package(CommonNet 1.0) 做兼容判断
    DESCRIPTION "Boost.Asio based C++ network communication library" # 工程描述，仅作文档用途
    LANGUAGES CXX) # 只启用 C++：纯 C++ 工程，不必让 CMake 额外探测 C 编译器

# ── 构建选项 ────────────────────────────────────────────
option(COMMONNET_BUILD_EXAMPLES "构建 Server/Client 示例可执行文件" ON) # 开关：默认编译 examples 下的 Server / Client 示例，可用 -DCOMMONNET_BUILD_EXAMPLES=OFF 关闭
# 用 -DBUILD_SHARED_LIBS=ON 可切换为动态库，默认为静态库

set(CMAKE_CXX_STANDARD 17) # 全工程统一 C++17（用到了 std::atomic、结构化绑定、if constexpr 等）
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 编译器不支持 C++17 时直接报错，而不是悄悄降级到旧标准
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES) # 单配置生成器（Ninja / Makefiles）下、且调用方没指定构建类型时
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE) # 兜底为 Release；VS 这类多配置生成器由 --config 决定，不走这一支
endif() # 构建类型兜底结束

# 安装目录变量（CMAKE_INSTALL_INCLUDEDIR / CMAKE_INSTALL_LIBDIR）在本文件前段
# 就会写进目标的 INSTALL_INTERFACE，故在定义目标之前先引入
include(GNUInstallDirs) # 引入 GNU 标准安装目录变量：include / lib / bin（Windows 上同样适用，且可被 -DCMAKE_INSTALL_LIBDIR=... 覆盖）

# 查找依赖库（均来自本机 vcpkg）
#   Utils —— 本工程的前置依赖库，由 vcpkg 的 utils 端口提供，导入目标 Utils::Utils
#   Boost —— 提供 asio / system / thread
find_package(Utils CONFIG REQUIRED) # 找 vcpkg 的 utils 包（share/utils/UtilsConfig.cmake），得到导入目标 Utils::Utils；找不到立即报错
find_package(Boost CONFIG REQUIRED COMPONENTS system thread) # 找 vcpkg 的 Boost（CONFIG 模式），只要 system / thread 两个组件；asio 是纯头文件，随 boost-headers 提供

# ── 公共属性封装（避免三个库重复书写）────────────────────
# 所有公开头文件都通过 asio，故 Windows 宏需以 PUBLIC 传播给使用者
function(common_net_options target) # 定义辅助函数：给目标统一加语言标准、Windows 宏与 MSVC 编译选项；形参 target 为目标名
    target_compile_features(${target} PUBLIC cxx_std_17) # 要求 C++17，并以 PUBLIC 传播：使用者无需自己再设 CMAKE_CXX_STANDARD
    if(WIN32) # 仅 Windows 需要的宏
        target_compile_definitions(${target} PUBLIC WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601) # PUBLIC：不下发会让使用者踩 winsock.h/winsock2.h 冲突；_WIN32_WINNT 保证 Vista+ 的 API 声明可见
    endif() # Windows 分支结束
    if(MSVC) # 仅 MSVC 认识的编译选项（GCC/Clang 传了会报错）
        target_compile_options(${target} PRIVATE /MP /FS /utf-8) # PRIVATE：/MP 多进程编译、/FS 串行化 PDB 写入、/utf-8 源码按 UTF-8 解析（含中文注释与字符串）
    endif() # MSVC 分支结束
endfunction() # common_net_options 定义结束

# ── 工具库：Utils（前置依赖库，改为消费本机 vcpkg 安装的 utils 包）──
# 旧写法：把 Utils 工程编出来的预编译静态库（lib/Utils.lib + lib/Utilsd.lib）
# 用 IMPORTED 目标 + 手写 Release/Debug 映射接进来，还得自己下发
# Utils_STATIC_DEFINE、自己安装库与头文件——源文件与产物容易脱节，
# Debug/Release 与 /MD、/MDd 的匹配也全靠人工维护。
#
# 现写法：直接消费 vcpkg 里的 utils 端口：
#     vcpkg install utils
#     find_package(Utils CONFIG REQUIRED)   ->   导入目标 Utils::Utils
# 导入目标自带全部必要信息，本工程不再手工下发任何 Utils 宏或包含目录：
#   - INTERFACE_INCLUDE_DIRECTORIES ：Utils.h / Message.h / UtilsExport.h
#   - INTERFACE_COMPILE_FEATURES    ：cxx_std_17
#   - INTERFACE_LINK_LIBRARIES      ：Threads::Threads
#   - 静态三元组（x64-windows-static）下另带 Utils_STATIC_DEFINE
#
# 默认三元组 x64-windows 下 Utils::Utils 是**动态库**：
#   Release -> lib/Utils.lib + bin/Utils.dll
#   Debug   -> debug/lib/Utils.lib + debug/bin/Utils.dll
# 两份产物由导入目标按当前配置自动选择，既不会出现「VS 多配置生成器下
# Debug 静默退回 Release」的隐患，也不必再操心 RuntimeLibrary 与
# _ITERATOR_DEBUG_LEVEL 是否一致（原来的 LNK2038 风险随之消失）。
# 正因如此，动态库场景**不能再定义 Utils_STATIC_DEFINE**：那会让 Utils_API
# 展开为空，Utils::serviceID 这类导出数据退化成普通 extern（链接期 LNK2019），
# 使用方若自己补定义更会直接触发 C2491 —— 该符号现在由 Utils.dll 导出。
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
target_link_libraries(CommonNetCore PUBLIC CommonUtils Boost::system Boost::thread) # PUBLIC：这三个依赖会随 Common::Net::Core 一起传递给下游
add_library(Common::Net::Core ALIAS CommonNetCore) # 构建期别名
# 导出名对齐构建期别名，使用者统一用 Common::Net::Core
set_target_properties(CommonNetCore PROPERTIES EXPORT_NAME Net::Core) # 导出名 Net::Core → 下游即 Common::Net::Core

# ── 服务端库 ────────────────────────────────────────────
add_library(CommonNetServer # 服务端库（默认静态库）
    Server/source/NetServer.cpp) # 实现文件：accept、会话管理、线程安全消息队列、WaitForMessage
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
