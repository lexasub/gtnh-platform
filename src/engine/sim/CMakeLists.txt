cmake_minimum_required(VERSION 3.20)

add_library(gtnh_engine_sim STATIC
    SimulationEngine.cpp
    PatternLibrary.cpp
)

# Public headers use <engine/sim/...> and <engine/registry/ItemId.h> path
# style, resolved from the src root (same convention as other lib targets).
target_include_directories(gtnh_engine_sim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

# SimulationEngine.cpp still wires machine systems (game/machines) which call
# back into simcore publishers — engine→game include paths are required until
# that wiring moves out of the engine (see openspec 3.6 note).
target_include_directories(gtnh_engine_sim PRIVATE
    ${CMAKE_SOURCE_DIR}/src/apps/simcore
    ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
)

target_link_libraries(gtnh_engine_sim PUBLIC
    gtnh_engine_registry
    spdlog::spdlog
)

target_link_libraries(gtnh_engine_sim PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
)
