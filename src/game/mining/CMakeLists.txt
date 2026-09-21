cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog REQUIRED)

add_library(gtnh_game_mining STATIC
    DrillSystem.cpp
    BatteryBufferSystem.cpp
    CreativeGeneratorSystem.cpp
    AdjacencyTransferSystem.cpp
)

target_include_directories(gtnh_game_mining PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/game
    ${CMAKE_SOURCE_DIR}/src/apps/simcore
)

target_link_libraries(gtnh_game_mining PUBLIC
    gtnh_game_recipes
    gtnh-net
    spdlog::spdlog
    EnTT::EnTT
)
