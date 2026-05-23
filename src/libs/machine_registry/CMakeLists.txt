cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog REQUIRED)

add_library(machine_registry STATIC
    MachineRegistry.cpp
)

target_include_directories(machine_registry PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(machine_registry PUBLIC
    spdlog::spdlog
)
