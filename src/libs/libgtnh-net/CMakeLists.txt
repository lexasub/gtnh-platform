cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog REQUIRED)

add_library(gtnh-net STATIC
    src/io_uring_context.cpp
    src/io_uring_connection.cpp
    src/tcp_connector.cpp
    src/frame.cpp
    src/server.cpp
    src/router_client.cpp
)

target_include_directories(gtnh-net PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(gtnh-net PUBLIC
    spdlog::spdlog
    uring
    pthread
)

# Tests
option(GTNH_NET_TESTS "Build libgtnh-net tests" ON)
if(GTNH_NET_TESTS)
    add_subdirectory(test)
endif()
