cmake_minimum_required(VERSION 3.10)
project(AsyCClient_CLI)

set(CMAKE_CXX_STANDARD 17)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# Include the third_party directory from the server for JSON support
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..) # For Protocol.h

# Create a library for the client logic
add_library(asyc_client AsyCClient.cpp)

# The CLI executable
add_executable(asyc_cli main.cpp)

# Link the CLI to the library and threads
target_link_libraries(asyc_cli asyc_client pthread)
