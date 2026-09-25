# Server / Client 示例程序：直接复用现有 main.cpp，链接对应库
add_executable(Server ${CMAKE_SOURCE_DIR}/Server/source/main.cpp)
target_link_libraries(Server PRIVATE CommonNetServer)
common_net_options(Server)

add_executable(Client ${CMAKE_SOURCE_DIR}/Client/source/main.cpp)
target_link_libraries(Client PRIVATE CommonNetClient)
common_net_options(Client)
