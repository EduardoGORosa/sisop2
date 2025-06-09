cmake_minimum_required(VERSION 3.10)
project(c_cloud_cpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread")

# onde estão seus headers
include_directories(include)

# executável do servidor
add_executable(server
    src/main_server.cpp
    src/Server.cpp
    src/Connection.cpp
    src/Packet.cpp
    src/FileManager.cpp
)

# executável do cliente
add_executable(client
    src/main_client.cpp
    src/Client.cpp
    src/Connection.cpp
    src/Packet.cpp
    src/FileManager.cpp
)

# link de threads
target_link_libraries(server pthread)
target_link_libraries(client pthread)

