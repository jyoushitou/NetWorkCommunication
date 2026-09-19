cmake_minimum_required(VERSION 3.16)

# 如果未指定工具链文件，优先使用 vcpkg（通过 VCPKG_ROOT 环境变量或项目内 vcpkg 目录）
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    else()
        message(FATAL_ERROR "未找到 vcpkg 工具链，请设置 VCPKG_ROOT 环境变量，或使用 -DCMAKE_TOOLCHAIN_FILE=... 指定工具链文件")
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
