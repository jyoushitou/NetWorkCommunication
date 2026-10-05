cmake_minimum_required(VERSION 3.16)

# CMake 4.x 已移除 FindBoost 模块，显式采用 NEW 行为，消除 CMP0167 回退警告
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# ── 通过 vcpkg 查找依赖（可移植写法，沿用原有子工程逻辑）─────────
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain file")
    else()
        message(FATAL_ERROR "未找到 vcpkg 工具链：请设置环境变量 VCPKG_ROOT，或使用 -DCMAKE_TOOLCHAIN_FILE=... 指定 vcpkg.cmake")
    endif()
endif()

set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "使用 vcpkg classic 已安装依赖" FORCE)

project(NetWorkCommunication
    VERSION 1.0.0
    DESCRIPTION "Boost.Asio based C++ network communication library"
    LANGUAGES CXX)

# ── 构建选项 ────────────────────────────────────────────
option(COMMONNET_BUILD_EXAMPLES "构建 Server/Client 示例可执行文件" ON)
# 用 -DBUILD_SHARED_LIBS=ON 可切换为动态库，默认为静态库

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 安装目录变量（CMAKE_INSTALL_INCLUDEDIR / CMAKE_INSTALL_LIBDIR）在本文件前段
# 就会写进目标的 INSTALL_INTERFACE，故在定义目标之前先引入
include(GNUInstallDirs)

# 查找 Boost 库
find_package(Boost CONFIG REQUIRED COMPONENTS system thread)

# ── 公共属性封装（避免三个库重复书写）────────────────────
# 所有公开头文件都通过 asio，故 Windows 宏需以 PUBLIC 传播给使用者
function(common_net_options target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    if(WIN32)
        target_compile_definitions(${target} PUBLIC WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /MP /FS /utf-8)
    endif()
endfunction()

# ── 工具库：Utils（前置依赖库，已由独立工程编译成库）──────
# Utils 是本工程的前置依赖库：由独立的 Utils 工程编译成静态库后发布，
# 本工程里只有它的发布产物——库文件与三个公开头文件
# lib/Utils.h、lib/Message.h、lib/UtilsExport.h（该库自身依赖的三方库已并入其中）。
# 因此这里不再编译 Utils 的源码，而是把这份「已经编译好的库」接入为库目标：
#   CommonUtilsPrebuilt —— 预编译静态库本体（IMPORTED 静态库，构建期真正参与链接）
#   CommonUtils         —— 对外接口目标，别名与导出名保持 Common::Utils 不变
#
# 关键：静态库的 .obj 里带着 /FAILIFMISMATCH 指令（RuntimeLibrary 与
# _ITERATOR_DEBUG_LEVEL），Debug 与 Release 两份产物互不兼容——Utils.h 的 ABI 里
# 有 std::string / std::vector<std::string> / std::function，跨 CRT 混链不只是
# 链接器报 LNK2038，绕过去还会踩坏堆。
# 又因为 VS 是多配置生成器：IMPORTED 目标若只设「配置无关」的 IMPORTED_LOCATION，
# Debug 会静默退回复用 Release 那份库。所以两份产物都必须有，并按配置分别导入：
#   UTILS_LIBRARY_RELEASE —— Release 版（MD  / _ITERATOR_DEBUG_LEVEL=0）
#   UTILS_LIBRARY_DEBUG   —— Debug   版（MDd / _ITERATOR_DEBUG_LEVEL=2）
# Debug 版缺失时的补救办法（在 Utils 工程自己的构建目录里执行）：
#   cmake --build <Utils 构建目录> --config Debug --target Utils
# 指向其他位置的同款发布包：
#   -DUTILS_LIBRARY_RELEASE=... -DUTILS_LIBRARY_DEBUG=... -DUTILS_INCLUDE_DIR=...
if(NOT UTILS_INCLUDE_DIR)
    set(UTILS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib"
        CACHE PATH "Utils 前置依赖库头文件目录（含 Utils.h / Message.h / UtilsExport.h）")
endif()

# 兼容旧变量名：早期版本只有一个 UTILS_LIBRARY，语义等同于 Release 版
if(UTILS_LIBRARY AND NOT UTILS_LIBRARY_RELEASE)
    set(UTILS_LIBRARY_RELEASE "${UTILS_LIBRARY}"
        CACHE FILEPATH "Utils 预编译静态库（Release 版）的完整路径")
    unset(UTILS_LIBRARY CACHE)
endif()

if(NOT UTILS_LIBRARY_RELEASE)
    set(UTILS_LIBRARY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/lib/Utils.lib"
        CACHE FILEPATH "Utils 预编译静态库（Release 版）的完整路径")
endif()
if(NOT UTILS_LIBRARY_DEBUG)
    set(UTILS_LIBRARY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/lib/Utilsd.lib"
        CACHE FILEPATH "Utils 预编译静态库（Debug 版）的完整路径")
endif()
if(NOT EXISTS "${UTILS_LIBRARY_RELEASE}")
    message(FATAL_ERROR "未找到 Utils Release 静态库：${UTILS_LIBRARY_RELEASE}"
        "（可用 -DUTILS_LIBRARY_RELEASE=... 指定）")
endif()
if(NOT EXISTS "${UTILS_INCLUDE_DIR}/Utils.h")
    message(FATAL_ERROR "未找到 Utils 头文件：${UTILS_INCLUDE_DIR}/Utils.h（可用 -DUTILS_INCLUDE_DIR=... 指定）")
endif()

# 预编译静态库本体：按配置导入 Release / Debug 两份产物
add_library(CommonUtilsPrebuilt STATIC IMPORTED)
set_target_properties(CommonUtilsPrebuilt PROPERTIES
    IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
    IMPORTED_LOCATION_RELEASE "${UTILS_LIBRARY_RELEASE}"
    # MinSizeRel / RelWithDebInfo 同样是 MD，与 Release 版同源，直接映射过去
    MAP_IMPORTED_CONFIG_MINSIZEREL Release
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)

if(EXISTS "${UTILS_LIBRARY_DEBUG}")
    set_target_properties(CommonUtilsPrebuilt PROPERTIES
        IMPORTED_LOCATION_DEBUG "${UTILS_LIBRARY_DEBUG}")
else()
    message(WARNING
        "未找到 Utils 的 Debug 版静态库：${UTILS_LIBRARY_DEBUG}\n"
        "Debug 配置将退回链接 Release 版 Utils.lib（${UTILS_LIBRARY_RELEASE}），\n"
        "链接阶段会因 RuntimeLibrary / _ITERATOR_DEBUG_LEVEL 不一致报 LNK2038。\n"
        "生成 Debug 版的命令（在 Utils 工程自己的构建目录里执行）：\n"
        "    cmake --build <Utils 构建目录> --config Debug --target Utils\n"
        "再把产出的 Debug/Utils.lib 放到上述路径，或用 -DUTILS_LIBRARY_DEBUG=<路径> 指定。")
endif()

# 对外接口目标：头文件即 lib/ 下的 Utils.h / Message.h / UtilsExport.h，按接口向下游传递
add_library(CommonUtils INTERFACE)
target_include_directories(CommonUtils SYSTEM INTERFACE
    $<BUILD_INTERFACE:${UTILS_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Utils>)
# 静态库消费方式与 Utils 工程自身的发布保持一致
target_compile_definitions(CommonUtils INTERFACE Utils_STATIC_DEFINE)
# 构建期链接预编译库本体；安装后按配置链接随包安装的那一份
# （Debug → Utilsd.lib，其余 → Utils.lib）
# （用 $<INSTALL_PREFIX> 而不是绝对路径，安装包整体搬迁后依然可用）
target_link_libraries(CommonUtils INTERFACE
    "$<BUILD_INTERFACE:CommonUtilsPrebuilt>"
    "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/$<IF:$<CONFIG:Debug>,Utilsd.lib,Utils.lib>>")
add_library(Common::Utils ALIAS CommonUtils)
set_target_properties(CommonUtils PROPERTIES EXPORT_NAME Utils)

# ── 核心库：协议 / 连接 ─────────────────────────────────
add_library(CommonNetCore
    connon/NetConnection.cpp)
common_net_options(CommonNetCore)
target_include_directories(CommonNetCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/connon>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Core>)
target_link_libraries(CommonNetCore PUBLIC CommonUtils Boost::system Boost::thread)
add_library(Common::Net::Core ALIAS CommonNetCore)
# 导出名对齐构建期别名，使用者统一用 Common::Net::Core
set_target_properties(CommonNetCore PROPERTIES EXPORT_NAME Net::Core)

# ── 服务端库 ────────────────────────────────────────────
add_library(CommonNetServer
    Server/source/NetServer.cpp)
common_net_options(CommonNetServer)
target_include_directories(CommonNetServer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Server/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Server>)
target_link_libraries(CommonNetServer PUBLIC CommonNetCore)
add_library(Common::Net::Server ALIAS CommonNetServer)
set_target_properties(CommonNetServer PROPERTIES EXPORT_NAME Net::Server)

# ── 客户端库 ────────────────────────────────────────────
add_library(CommonNetClient
    Client/source/NetClient.cpp)
common_net_options(CommonNetClient)
target_include_directories(CommonNetClient PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Client/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Client>)
target_link_libraries(CommonNetClient PUBLIC CommonNetCore)
add_library(Common::Net::Client ALIAS CommonNetClient)
set_target_properties(CommonNetClient PROPERTIES EXPORT_NAME Net::Client)

# ── 示例可执行文件 ──────────────────────────────────────
if(COMMONNET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── 安装与导出 ──────────────────────────────────────────
include(CMakePackageConfigHelpers)

# Utils 是预编译静态库：CommonUtils 只承载接口，故库本体需单独安装，
# 这样安装后的 Common::Utils 才能链到包内真实存在的 Utils.lib
install(TARGETS CommonUtils CommonNetCore CommonNetServer CommonNetClient
    EXPORT CommonNetTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(FILES "${UTILS_LIBRARY_RELEASE}"
    DESTINATION ${CMAKE_INSTALL_LIBDIR})
# Debug 版单独落盘为 Utilsd.lib，与 Release 版同目录共存、互不覆盖
if(EXISTS "${UTILS_LIBRARY_DEBUG}")
    install(FILES "${UTILS_LIBRARY_DEBUG}"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RENAME Utilsd.lib)
endif()

# Utils 前置库的三个头文件：Message.h 被 Utils.h 直接包含，必须与 Utils.h 同目录，
# 安装后的 Common/Utils/Utils.h 才能自包含；
# 注意只能安装这一份——#pragma once 按路径去重，同一头文件放两处会重复定义（C2011）
install(FILES
    ${UTILS_INCLUDE_DIR}/Utils.h
    ${UTILS_INCLUDE_DIR}/Message.h
    ${UTILS_INCLUDE_DIR}/UtilsExport.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Utils)

# 核心头文件：使用 Common::Net::Core 时其接口会带上 Common/Utils，
# 故 #include "Message.h" 依旧能找到上面安装的那一份
install(FILES
    connon/NetConnection.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Core)

install(FILES
    Server/include/NetServer.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Server)

install(FILES
    Client/include/NetClient.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Client)

install(EXPORT CommonNetTargets
    FILE CommonNetTargets.cmake
    NAMESPACE Common::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CommonNet)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CommonNetConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CommonNetConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CommonNet)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/CommonNetConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/CommonNetConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/CommonNetConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CommonNet)
