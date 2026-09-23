cmake_minimum_required(VERSION 3.16)

# ── 通过 vcpkg 查找依赖（可移植写法）───────────────────
# 不硬编码任何本机路径，使用 vcpkg 官方推荐机制：
#   1) 命令行传入 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   2) 或设置环境变量 VCPKG_ROOT
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain file")
    else()
        message(FATAL_ERROR "未找到 vcpkg 工具链：请设置环境变量 VCPKG_ROOT，或使用 -DCMAKE_TOOLCHAIN_FILE=... 指定 vcpkg.cmake")
    endif()
endif()

project(Server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 下避免 winsock.h 与 winsock2.h 冲突
if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif()

# MSVC 下启用多进程并行编译、解决 PDB 文件争用问题，并要求以 UTF-8 解析源码
if(MSVC)
    add_compile_options(/MP /FS /utf-8)
endif()

# 查找 Boost 库 (asio, system, thread)
find_package(Boost REQUIRED COMPONENTS system thread)

# 共享代码目录
set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../connon)

# 源文件
set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/body/NetServer.cpp
    ${COMMON_DIR}/NetConnection.cpp
    ${COMMON_DIR}/Utils.cpp
)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${COMMON_DIR}
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_link_libraries(${PROJECT_NAME}
    Boost::system
    Boost::thread
)
