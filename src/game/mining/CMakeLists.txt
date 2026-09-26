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

# AdjacencyTransferSystem unit tests — the three-pass heat pipeline (adjacent
# transfer, overheat detection, environment cooling). Self-contained: the
# system needs only EnTT, the ECS components, MachineRegistry and a publisher
# double, so the test links gtnh_game_mining directly. No GTest — the suite
# uses the project harness in src/engine/net/test/test.h, because the repo has
# no GTest dependency and CI builds with -Werror.
if(BUILD_TESTING)
    add_executable(adjacency_transfer_system_test
        test/test_adjacency_transfer_system.cpp
    )
    target_include_directories(adjacency_transfer_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_compile_definitions(adjacency_transfer_system_test PRIVATE
        GTNH_MACHINES_YAML="${CMAKE_SOURCE_DIR}/src/content/data/registry/machines.yaml"
    )
    target_link_libraries(adjacency_transfer_system_test PRIVATE
        gtnh_game_mining
        gtnh_engine_sim
        EnTT::EnTT
        spdlog::spdlog
    )
    add_test(NAME adjacency_transfer_system_test COMMAND adjacency_transfer_system_test)
endif()
