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
#   include/NetServer.h                    -> 下游只加一个 -I<lib>/include 即可（三个头文件名互不冲突）
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

set(COMMONNET_STAGE_INCLUDEDIR "$<TARGET_FILE_DIR:CommonNetCore>/include") # 交付目录：与 .lib 同级
set(COMMONNET_STAGE_COMMANDS) # 累积同步命令（下面按头文件逐条追加）
foreach(header_entry IN LISTS COMMONNET_PUBLIC_HEADERS) # 遍历三个公开头文件
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
