cmake_minimum_required(VERSION 3.21)
project("echo-server-TCP")

set(CXX_MINIMUM_STANDARD 17)
set(CXX_STANDARD_REQUIRED ON)

add_executable(server server.cpp)
add_executable(client client.cpp)
