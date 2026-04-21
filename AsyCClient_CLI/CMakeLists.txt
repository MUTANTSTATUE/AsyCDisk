cmake_minimum_required(VERSION 3.10)
project(AsyCClient_CLI)

set(CMAKE_CXX_STANDARD 17)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# Include the third_party directory from the server for JSON support
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

add_executable(asyc_cli main.cpp)

# The CLI client uses standard Linux sockets, no extra libs needed
