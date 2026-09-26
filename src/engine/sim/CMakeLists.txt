cmake_minimum_required(VERSION 3.20)

find_package(yaml-cpp REQUIRED)

add_library(gtnh_engine_sim STATIC
    SimulationEngine.cpp
    PatternLibrary.cpp
    MachineRegistry.cpp
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
    yaml-cpp::yaml-cpp
)

# engine_sim_components_test — the first unit-test target for engine/sim
# (closes beads gp-s5r's coverage gap for components/). Covers all 16 ECS
# component headers in components/ in one executable: defaults, field
# semantics, and every declared helper/conversion.
#
# All 16 headers are header-only (components/ has no .cpp), so this target
# deliberately does NOT link gtnh_engine_sim. Doing so would drag in
# gtnh_engine_sim's PRIVATE yaml-cpp and force a per-subdir
# find_package(yaml-cpp) here — the trap another agent already hit. The only
# things the headers pull in are <engine/registry/ItemId.h> (via
# PatternLibrary.h) and <common/ResourcePort.h> (via RecipeProgress.h's
# __has_include probe), both fully header-only, plus the harness header.
#
# Harness: the project's own CHECK/TEST macros from src/engine/net/test/test.h.
# GoogleTest is FORBIDDEN — it is absent from conanfile.txt, CI does not
# install libgtest-dev, and CI builds Release with a global -Werror. A
# find_package(GTest QUIET) guard would be worse than nothing: the test would
# silently stop registering on CI instead of failing.
if(BUILD_TESTING)
    add_executable(engine_sim_components_test
        test/EngineSimComponents_test.cpp
    )
    target_include_directories(engine_sim_components_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/engine/net
    )
    target_link_libraries(engine_sim_components_test PRIVATE
        gtnh_engine_registry
    )
    add_test(NAME engine_sim_components_test COMMAND engine_sim_components_test)
endif()


