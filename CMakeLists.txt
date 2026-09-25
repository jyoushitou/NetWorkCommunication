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

# ── 工具库：Utils ───────────────────────────────────────
add_library(CommonUtils
    connon/Utils.cpp)
common_net_options(CommonUtils)
target_include_directories(CommonUtils PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/connon>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Utils>)
target_link_libraries(CommonUtils PUBLIC Boost::system Boost::thread)
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
    Server/source/body/NetServer.cpp)
common_net_options(CommonNetServer)
target_include_directories(CommonNetServer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Server/source/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Server>)
target_link_libraries(CommonNetServer PUBLIC CommonNetCore)
add_library(Common::Net::Server ALIAS CommonNetServer)
set_target_properties(CommonNetServer PROPERTIES EXPORT_NAME Net::Server)

# ── 客户端库 ────────────────────────────────────────────
add_library(CommonNetClient
    Client/source/body/NetClient.cpp)
common_net_options(CommonNetClient)
target_include_directories(CommonNetClient PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Client/source/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Client>)
target_link_libraries(CommonNetClient PUBLIC CommonNetCore)
add_library(Common::Net::Client ALIAS CommonNetClient)
set_target_properties(CommonNetClient PROPERTIES EXPORT_NAME Net::Client)

# ── 示例可执行文件 ──────────────────────────────────────
if(COMMONNET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── 安装与导出 ──────────────────────────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS CommonUtils CommonNetCore CommonNetServer CommonNetClient
    EXPORT CommonNetTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(FILES
    connon/Utils.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Utils)

install(FILES
    connon/Message.h
    connon/NetConnection.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Core)

install(FILES
    Server/source/include/NetServer.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Common/Net/Server)

install(FILES
    Client/source/include/NetClient.h
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
