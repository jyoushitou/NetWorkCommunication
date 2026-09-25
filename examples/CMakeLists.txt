# Server / Client 示例程序：main.cpp 统一放在 examples 下管理，链接对应库
add_executable(Server ${CMAKE_CURRENT_SOURCE_DIR}/Server/main.cpp)
target_link_libraries(Server PRIVATE CommonNetServer)
common_net_options(Server)

add_executable(Client ${CMAKE_CURRENT_SOURCE_DIR}/Client/main.cpp)
target_link_libraries(Client PRIVATE CommonNetClient)
common_net_options(Client)
