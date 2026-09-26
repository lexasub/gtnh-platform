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
    EnTT::EnTT
)

# ExplosionSystem unit tests — the CRITICAL-overheat fuse and the block it
# destroys. Self-contained: the system only needs EnTT, the ECS components, and
# an IEventPublisher double, so it links gtnh_game_machines directly (no
# LINK_GROUP/simcored pull-in, which keeps the test target cheap to build).
if(BUILD_TESTING)
    add_executable(explosion_system_test
        test/test_explosion_system.cpp
    )
    target_include_directories(explosion_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(explosion_system_test PRIVATE
        gtnh_game_machines
        gtnh_engine_sim
        EnTT::EnTT
        spdlog::spdlog
    )
    add_test(NAME explosion_system_test COMMAND explosion_system_test)

    # TransformerSystem unit tests (gp-ghyk) — voltage-tier conversion in both
    # directions. Self-contained: the system reads four ECS components and
    # guards every pipe-client call, so a null publisher and a null
    # PipeEnergyClient exercise the pure state-transition path with no router,
    # no socket and no wall clock. Same harness as explosion_system_test (no
    # GTest: the repo has no GTest dependency and CI builds with -Werror).
    add_executable(transformer_system_test
        test/test_transformer_system.cpp
    )
    target_include_directories(transformer_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(transformer_system_test PRIVATE
        # Circular static deps: game/machines calls into simcore's
        # PipeEnergyClient/FluidClient (Network/*.cpp inside libsimcored.a)
        # while simcored calls the machine systems back. CMake's LINK_GROUP
        # cannot express this here (it introduces a dependency cycle through
        # simcored's PUBLIC game/* deps), so the archives are listed once and
        # then repeated — the classic way to let the linker rescan them.
        gtnh_game_machines
        gtnh_engine_sim
        gtnh_game_recipes
        gtnh-net
        simcored
        gtnh_game_machines
        gtnh_engine_sim
        gtnh_game_recipes
        gtnh-net
        simcored
        EnTT::EnTT
        spdlog::spdlog
    )
    add_test(NAME transformer_system_test COMMAND transformer_system_test)

    # SteamTurbineSystem unit tests (gp-q692) — steam consumption and EU
    # generation. Null energy/fluid clients: the system guards both, so the
    # conversion path runs with no router, socket or wall clock.
    add_executable(steam_turbine_system_test
        test/test_steam_turbine_system.cpp
    )
    target_include_directories(steam_turbine_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(steam_turbine_system_test PRIVATE
        # Same circular-static-archive situation as transformer_system_test:
        # libsimcored.a holds the Network clients the machine systems call.
        gtnh_game_machines
        gtnh_engine_sim
        gtnh_game_recipes
        gtnh-net
        simcored
        gtnh_game_machines
        gtnh_engine_sim
        gtnh_game_recipes
        gtnh-net
        simcored
        EnTT::EnTT
        spdlog::spdlog
    )
    add_test(NAME steam_turbine_system_test COMMAND steam_turbine_system_test)

    # RotareGeneratorSystem unit tests (gp-kxj5) — spin rate, duration and the
    # stop conditions. Null pipe client: the system guards every publish call,
    # so the conversion path runs with no router, socket or wall clock.
    add_executable(rotare_generator_system_test
        test/test_rotare_generator_system.cpp
    )
    target_include_directories(rotare_generator_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(rotare_generator_system_test PRIVATE
        # Same circular-static-archive situation as transformer_system_test.
        gtnh_game_machines
        gtnh_engine_sim
        gtnh_game_recipes
        gtnh-net
        simcored
        gtnh_game_machines
        gtnh_engine_sim
        gtnh_game_recipes
        gtnh-net
        simcored
        EnTT::EnTT
        spdlog::spdlog
    )
    add_test(NAME rotare_generator_system_test COMMAND rotare_generator_system_test)
endif()
