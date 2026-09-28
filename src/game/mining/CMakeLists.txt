cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spdlog REQUIRED)

add_library(gtnh_game_mining STATIC
    DrillSystem.cpp
    BatteryBufferSystem.cpp
    CreativeGeneratorSystem.cpp
    CreativeFluidSystem.cpp
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

    # mining_systems_test — DrillSystem, BatteryBufferSystem and
    # CreativeGeneratorSystem (issues gp-fcvf / gp-lcuf / gp-ht7r), plus the
    # server-side creative ITEM_ACTION inventory grant that CreativeGenerator
    # does not own. Same harness as above: the project CHECK/TEST macros from
    # src/engine/net/test/test.h, no GoogleTest anywhere in the build.
    #
    # DrillSystem needs content::oreDropTable / content::kOre* (gtnh_content)
    # and the ItemEnergyStorage tool table; the grant half needs
    # PlayerInventoryStore + PlayerActionDispatcher and the generated
    # FlatBuffers headers (simcored_fbs).
    add_executable(mining_systems_test
        test/test_mining_systems.cpp
        # PipeEnergyClient and IoUringRouterClient are compiled from source
        # rather than linked from the simcored static lib, which would drag in
        # main.cpp.o and a second main(). Same pattern as
        # src/game/storage/CMakeLists.txt:chest_state_manager_test. Both are
        # pure socket wrappers and the test drives them UNCONNECTED, so no
        # thread, socket or network is ever created.
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/PipeEnergyClient.cpp
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/clients/IoUringRouterClient.cpp
    )
    target_include_directories(mining_systems_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
        ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
    )
    target_link_libraries(mining_systems_test PRIVATE
        gtnh_game_mining
        gtnh_game_actions
        gtnh_game_storage
        gtnh_content
        gtnh_engine_sim
        gtnh-net
        EnTT::EnTT
        flatbuffers::flatbuffers
        asio::asio
        spdlog::spdlog
    )
    add_dependencies(mining_systems_test simcored_fbs)
    add_test(NAME mining_systems_test COMMAND mining_systems_test)

    # creative_fluid_system_test — CreativeFluidSystem, the infinite OIL and
    # WATER sources (machines.yaml 1110:100:15 / 1110:100:16, gp-asy3).
    # Same project CHECK/TEST harness, no GoogleTest anywhere in the build.
    add_executable(creative_fluid_system_test
        test/test_creative_fluid_system.cpp
        # FluidClient and IoUringRouterClient are compiled from source rather
        # than linked from the simcored static lib, which would drag in
        # main.cpp.o and a second main(). Same pattern as mining_systems_test
        # above. Both are pure socket wrappers and the test drives them
        # UNCONNECTED, so no thread, socket or network I/O ever happens.
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/FluidClient.cpp
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/clients/IoUringRouterClient.cpp
    )
    target_include_directories(creative_fluid_system_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
        ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
    )
    target_link_libraries(creative_fluid_system_test PRIVATE
        gtnh_game_mining
        gtnh_engine_sim
        gtnh-net
        EnTT::EnTT
        spdlog::spdlog
        flatbuffers::flatbuffers
    )
    add_dependencies(creative_fluid_system_test simcored_fbs)
    add_test(NAME creative_fluid_system_test COMMAND creative_fluid_system_test)
endif()
