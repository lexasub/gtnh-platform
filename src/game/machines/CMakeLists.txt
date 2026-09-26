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

    # EBFSystem unit tests (gp-r3vd) — EU consumption, HU output and the
    # stall conditions for the electric / hot blast furnace. The system is
    # driven through the controllers_ map the production wiring hands it
    # (main.cpp passes SimulationEngine::getControllers()), so the test owns a
    # local map plus a synthetic recipe file and needs no router, socket or
    # wall clock. Same harness as the suites above: no GoogleTest.
    add_executable(ebf_system_test
        test/test_ebf_system.cpp
    )
    target_include_directories(ebf_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(ebf_system_test PRIVATE
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
        yaml-cpp::yaml-cpp
        flatbuffers::flatbuffers
    )
    add_test(NAME ebf_system_test COMMAND ebf_system_test)

    # LCRSystem unit tests (gp-mb2d) — the multiblock validation, tier and
    # energy-hatch fallback questions from the openspec task
    # add-scaled-energy-hatch-multiblock-verification 1.3. Same harness and
    # same isolated wiring as the EBF suite above: a local controller map, a
    # synthetic recipe file and a null pipe client, so no router, socket or
    # wall clock is involved.
    add_executable(lcr_system_test
        test/test_lcr_system.cpp
    )
    target_include_directories(lcr_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(lcr_system_test PRIVATE
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
        yaml-cpp::yaml-cpp
        flatbuffers::flatbuffers
    )
    add_test(NAME lcr_system_test COMMAND lcr_system_test)

    # BoilerSystem + LargeBoilerSystem unit tests (gp-g850) — the HEAT ->
    # STEAM conversion of the heat boiler and the solid-fuel burn, steam
    # saturation and overheat path of the large boiler. Both are registered
    # unconditionally in main.cpp:105 / :494, so neither is dead code; this
    # suite drives them straight off the registry view / controller map with
    # null pipe, fluid and port clients, so no router, socket or wall clock.
    add_executable(boiler_system_test
        test/test_boiler_system.cpp
    )
    target_include_directories(boiler_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(boiler_system_test PRIVATE
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
        yaml-cpp::yaml-cpp
        flatbuffers::flatbuffers
    )
    add_test(NAME boiler_system_test COMMAND boiler_system_test)

    # MachineSystem unit tests (gp-015h tick/state publication, gp-s9to
    # machine-tag filtering and role dispatch) — the generic single-block
    # machine loop, previously untested. Same harness and same isolated wiring
    # as the EBF suite above: a synthetic recipe file, a non-null (empty)
    # MachineRegistry, and null pipe/item/session/reservation clients, so no
    # router, socket or wall clock is involved. The one non-null client is a
    # FluidClient subclass whose two virtual overrides record calls and never
    # reach the (never connected) router.
    add_executable(machine_system_test
        test/test_machine_system.cpp
    )
    target_include_directories(machine_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(machine_system_test PRIVATE
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
        yaml-cpp::yaml-cpp
        flatbuffers::flatbuffers
        # MachineSystem.cpp:674 calls PublishFullInventory, which lives in
        # gtnh_game_storage (ChestStateManager.cpp) and needs
        # EntityStateStoreClient from libsimcored.a. gtnh_game_storage is pulled
        # in transitively (via gtnh_game_actions) AFTER libsimcored on the link
        # line, so those refs are still unresolved at the point ld scans it.
        # One more rescan closes it.
        gtnh_game_storage
        simcored
    )
    add_test(NAME machine_system_test COMMAND machine_system_test)
endif()
