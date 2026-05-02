cmake_minimum_required(VERSION 3.14)
project(AsyCDisk LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(FetchContent)
set(JSON_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party)
if(NOT EXISTS ${JSON_INCLUDE_DIR}/nlohmann/json.hpp)
    file(DOWNLOAD 
        https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp
        ${JSON_INCLUDE_DIR}/nlohmann/json.hpp
    )
endif()
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${JSON_INCLUDE_DIR})


# 2. Fetch spdlog
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
)
FetchContent_MakeAvailable(spdlog)

# 3. Fetch libuv
FetchContent_Declare(libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG v1.46.0
)
FetchContent_MakeAvailable(libuv)

# 4. Find SQLite3
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

# Include directories
include_directories(include ${OPENSSL_INCLUDE_DIR})

# Add main executable
add_executable(AsyCDisk 
    src/main.cpp
    src/Logger.cpp
    src/EventLoop.cpp
    src/TcpServer.cpp
    src/Session.cpp
    src/Database.cpp
    src/CryptoUtils.cpp
    src/Config.cpp
)

# Link libraries
target_link_libraries(AsyCDisk PRIVATE
    uv_a
    spdlog::spdlog
    nlohmann_json
    SQLite3::SQLite3
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

# 自动将配置文件拷贝到生成目录
configure_file(
    ${CMAKE_SOURCE_DIR}/config.json 
    ${CMAKE_BINARY_DIR}/config.json 
    COPYONLY
)
