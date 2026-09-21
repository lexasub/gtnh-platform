cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)

add_library(gtnh_game_machines STATIC
    MachineSystem.cpp
    BoilerSystem.cpp
    GeneratorSystem.cpp
    TransformerSystem.cpp
    SteamTurbineSystem.cpp
    RotareGeneratorSystem.cpp
    LargeBoilerSystem.cpp
    LCRSystem.cpp
    EBFSystem.cpp
    ExplosionSystem.cpp
)

# Note: machine systems still reference simulation_core headers
# (Network/, ECS/components/, RecipeManager/) — that coupling is resolved
# in a later phase when engine/sim extraction lands.
target_include_directories(gtnh_game_machines PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/game
    ${CMAKE_SOURCE_DIR}/src/apps/simcore
    # generated FlatBuffers headers (pipe_network_generated.h etc.) needed
    # transitively via common/ResourcePortClient.h — produced by simcored_fbs
    ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
)

add_dependencies(gtnh_game_machines simcored_fbs)

target_link_libraries(gtnh_game_machines PUBLIC
    gtnh_wire
    gtnh_engine_sim
    gtnh_game_recipes
    gtnh-net
    spdlog::spdlog
    yaml-cpp::yaml-cpp
)
