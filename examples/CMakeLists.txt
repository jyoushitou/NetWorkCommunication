# Server / Client 示例程序：main.cpp 统一放在 examples 下管理，链接对应库
add_executable(Server ${CMAKE_CURRENT_SOURCE_DIR}/Server/main.cpp)  # 生成服务端示例可执行文件 Server（MSVC 下即 Server.exe，输出到 <构建目录>/examples/<配置>/）
target_link_libraries(Server PRIVATE CommonNetServer)  # PRIVATE：只有示例自己需要服务端库；可执行文件没有下游，不必向外传播
common_net_options(Server)  # 套用根 CMakeLists.txt 里的公共属性（C++17 / WIN32_LEAN_AND_MEAN / _WIN32_WINNT / MSVC /MP /FS /utf-8）

add_executable(Client ${CMAKE_CURRENT_SOURCE_DIR}/Client/main.cpp)  # 生成客户端示例可执行文件 Client（MSVC 下即 Client.exe）
target_link_libraries(Client PRIVATE CommonNetClient)  # PRIVATE：只链接客户端库（它已 PUBLIC 依赖 CommonNetCore → CommonUtils → Utils::Utils）
common_net_options(Client)  # 同样套用公共属性，保证两个示例的编译选项完全一致
