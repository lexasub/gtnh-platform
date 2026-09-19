cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gtnh_content STATIC
    content.cpp
)

# content.h uses <engine/registry/ItemId.h> path style, resolved from src root
target_include_directories(gtnh_content PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(gtnh_content PUBLIC
    gtnh_engine_registry
)
