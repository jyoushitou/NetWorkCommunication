# 示例程序：Server（TCP 服务端）/ Client（TCP 客户端）/ HttpServer（HTTP 服务端）
# 每个示例只链接对应的库；公共编译选项统一由 common_net_options() 下发
# （C++17 / WIN32_LEAN_AND_MEAN / _WIN32_WINNT / MSVC /MP /FS /utf-8 —— /utf-8 必须有，
#   否则源码里的中文注释会被按本地代码页解析，可能把下一行吞进注释）

add_executable(Server ${CMAKE_CURRENT_SOURCE_DIR}/Server/main.cpp) # 生成服务端示例可执行文件 Server（MSVC 下即 Server.exe，输出到 <构建目录>/examples/<配置>/）
target_link_libraries(Server PRIVATE CommonNetServer) # PRIVATE：只有示例自己需要服务端库；可执行文件没有下游，不必向外传播
common_net_options(Server) # 套用根 CMakeLists.txt 里的公共属性

add_executable(Client ${CMAKE_CURRENT_SOURCE_DIR}/Client/main.cpp) # 生成客户端示例可执行文件 Client（MSVC 下即 Client.exe）
target_link_libraries(Client PRIVATE CommonNetClient) # PRIVATE：只链接客户端库（它已 PUBLIC 依赖 CommonNetCore → CommonUtils → Utils::Utils）
common_net_options(Client) # 同样套用公共属性，保证示例之间编译选项完全一致

# HTTP 示例依赖 CommonNetHttpServer 目标，只有开关打开时才存在（见根 CMakeLists.txt 的 COMMONNET_BUILD_HTTP_SERVER）
if(COMMONNET_BUILD_HTTP_SERVER)
    add_executable(HttpServer ${CMAKE_CURRENT_SOURCE_DIR}/Httpserver/main.cpp) # 生成 HTTP 服务端示例可执行文件 HttpServer.exe
    target_link_libraries(HttpServer PRIVATE CommonNetHttpServer) # PRIVATE：HTTP 库已 PUBLIC 依赖 CommonNetServer → CommonNetCore → CommonUtils
    common_net_options(HttpServer) # 套用公共属性
endif()
