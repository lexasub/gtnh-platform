# Player actions — break/place/interact handlers, wrench + drill tools.
# Extracted from simcore (game rules depend on engine, not vice versa).
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gtnh_game_actions STATIC
    ActionContext.cpp
    ActionDispatcher.cpp
    CasRunner.cpp
    PlayerActionDispatcher.cpp
    SetBlockCASHandler.cpp
    MachineSlotHandler.cpp
    handlers/BreakBlockHandler.cpp
    handlers/ChestInteractHandler.cpp
    handlers/MachineInteractHandler.cpp
    handlers/PlaceBlockHandler.cpp
    handTool/WrenchHandler.cpp
    handTool/ElectricDrillHandler.cpp
    handTool/ToolActionHandler.cpp
    handTool/WrenchActionHandler.cpp
    handTool/PipeWrenchResponseHandler.cpp
)

# Quoted includes reach simcore internals (Network/, Storage/, Common/) and
# simcore's generated FlatBuffers headers — same explicit pattern as
# gtnh_game_machines.
target_include_directories(gtnh_game_actions PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/apps/simcore
    ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
)

add_dependencies(gtnh_game_actions simcored_fbs)

target_link_libraries(gtnh_game_actions PUBLIC
    gtnh_wire
    gtnh_engine_registry
    gtnh_engine_sim
    gtnh_game_quests
    gtnh-net
    EnTT::EnTT
)

target_link_libraries(gtnh_game_actions PRIVATE
    spdlog::spdlog
    asio::asio
)

# action_dispatch_test — the action dispatch seam (issue gp-0xml):
# ActionDispatcher (the handler-priority tuple), ActionContext (the immutable
# action snapshot) and PlayerActionDispatcher (the legacy PlayerAction switch).
# Uses the project's own CHECK/TEST harness (src/engine/net/test/test.h, mirrored
# by src/game/machines/test/test_explosion_system.cpp); the project has no
# GoogleTest dependency and CI does not install libgtest-dev, so a
# find_package(GTest) would make this test silently vanish from CI.
#
# Self-contained: every dependency is an in-process double (fake ChunkStore
# repository, recording IEventPublisher, real SimulationEngine + MachineRegistry
# loaded from the real machines.yaml). No sockets, no threads, no sleeps.
if(BUILD_TESTING)
    add_executable(action_dispatch_test
        test/test_action_dispatch.cpp
    )
    target_compile_definitions(action_dispatch_test PRIVATE
        MACHINES_YAML="${CMAKE_SOURCE_DIR}/src/content/data/registry/machines.yaml"
    )
    target_include_directories(action_dispatch_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
        ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
    )
    target_link_libraries(action_dispatch_test PRIVATE
        gtnh_game_actions
        gtnh_game_storage
        gtnh_game_world
        gtnh_engine_sim
        EnTT::EnTT
        flatbuffers::flatbuffers
        spdlog::spdlog
    )
    add_dependencies(action_dispatch_test simcored_fbs)
    add_test(NAME action_dispatch_test COMMAND action_dispatch_test)

    # wrench_server_test — the server half of the pipe wrench (issue gp-r946):
    # WrenchHandler::cycleFace (the side-role cycle, including multiblock hatch
    # slots) and WrenchActionHandler::handle (the wire decode, the 200 ms
    # cooldown and the pipe/cable meta toggle). The client half already has
    # five passing ctest targets; this closes the asymmetry.
    #
    # Same harness as action_dispatch_test: the project CHECK/TEST macros, no
    # GoogleTest anywhere in the build.
    #
    # The meta expectations are the ones
    # src/apps/game_client/tests/WrenchMetaLink_test.cpp already asserts (they
    # were themselves taken from live server logs), so the server is checked
    # against the client's contract rather than a fresh invention.
    #
    # EntityStateStoreClient is compiled from source rather than linked from the
    # simcored static lib (which would drag in main.cpp.o and a second main()),
    # same as src/game/storage/CMakeLists.txt:chest_state_manager_test. The
    # test never connects it, so no socket, thread or network is created.
    add_executable(wrench_server_test
        test/test_wrench_server.cpp
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/clients/EntityStateStoreClient.cpp
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/clients/IoUringRouterClient.cpp
    )
    target_include_directories(wrench_server_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
        ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
    )
    target_link_libraries(wrench_server_test PRIVATE
        gtnh_game_actions
        gtnh_game_storage
        gtnh_engine_sim
        gtnh_engine_registry
        gtnh-net
        EnTT::EnTT
        flatbuffers::flatbuffers
        asio::asio
        spdlog::spdlog
    )
    add_dependencies(wrench_server_test simcored_fbs)
    add_test(NAME wrench_server_test COMMAND wrench_server_test)
endif()
