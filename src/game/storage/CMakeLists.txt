add_library(gtnh_game_storage STATIC
    ChunkStoreRepository.cpp
    PlayerInventoryStore.cpp
    InventoryLoadHandler.cpp
    InventoryActionHandler.cpp
    ChestStateManager.cpp
    PlayerJoinedHandler.cpp
)

target_include_directories(gtnh_game_storage PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
)

add_dependencies(gtnh_game_storage simcored_fbs)

target_link_libraries(gtnh_game_storage PRIVATE
    asio::asio
)

target_link_libraries(gtnh_game_storage PUBLIC
    gtnh_engine_sim
    gtnh_wire
    gtnh-net
    EnTT::EnTT
    spdlog::spdlog
    flatbuffers::flatbuffers
)

# chest_state_manager_test — unit tests for ChestStateManager (the per-chest
# persistence layer behind ContainerSessionRegistry) and for
# ChestInteractHandler (the right-click gate). Uses the project's own
# CHECK/TEST harness (see src/engine/net/test/test.h,
# src/game/world/test/BlockTransforms_test.cpp and
# src/game/machines/test/test_explosion_system.cpp); the project has NO
# GoogleTest dependency — gtest is absent from conanfile.txt, CI does not
# install libgtest-dev, and CI builds Release with a global -Werror, so a
# find_package(GTest) guard would make the test silently vanish there.
#
# ChestInteractHandler is owned by gtnh_game_actions, so this test links that
# too. It compiles EntityStateStoreClient/IoUringRouterClient from source
# rather than linking the simcored static lib, which would drag in main.cpp.o
# and a second main(). Both are pure socket wrappers: the test drives them
# unconnected, so no thread, socket or network is ever created.
if(BUILD_TESTING)
    # gtnh_engine_sim links yaml-cpp PRIVATE (MachineRegistry.cpp), so the
    # yaml-cpp symbols the linker needs must be findable from THIS directory
    # too. Same find_package the other game subdirs use.
    find_package(yaml-cpp REQUIRED)

    add_executable(chest_state_manager_test
        test/test_chest_state_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/clients/EntityStateStoreClient.cpp
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/clients/IoUringRouterClient.cpp
    )
    target_include_directories(chest_state_manager_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(chest_state_manager_test PRIVATE
        gtnh_game_storage
        gtnh_game_actions
        gtnh_engine_sim
        gtnh_engine_registry
        gtnh-net
        EnTT::EnTT
        spdlog::spdlog
        flatbuffers::flatbuffers
        asio::asio
        yaml-cpp::yaml-cpp
        uring
        pthread
    )
    add_dependencies(chest_state_manager_test simcored_fbs)
    add_test(NAME chest_state_manager_test COMMAND chest_state_manager_test)
endif()

# chunk_store_repository_test — unit tests for ChunkStoreRepository, the
# simcore→ChunkStore seam (issue gp-897f). Uses the project's own CHECK/TEST
# harness (see src/engine/net/test/test.h); the project has NO GoogleTest
# dependency — gtest is absent from conanfile.txt, CI does not install
# libgtest-dev, and CI builds Release with a global -Werror, so a
# find_package(GTest) guard would make the test silently vanish from CI.
#
# ChunkStoreRepository takes a std::shared_ptr<IoUringChunkClient> — a
# CONCRETE, non-virtual class — so it cannot be substituted at runtime. This
# target links a link-time STUB of the three members the repository calls
# (test/stub_chunk_client.cpp) instead of the real client, which keeps the
# test free of any socket, thread, LMDB or cluster. The stub's header is
# included by the test TU; the real IoUringChunkClient.cpp is deliberately NOT
# added here (it would be a duplicate-symbol clash against the stub).
if(BUILD_TESTING)
    add_executable(chunk_store_repository_test
        test/test_chunk_store_repository.cpp
        test/stub_chunk_client.cpp
    )
    target_include_directories(chunk_store_repository_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
        ${CMAKE_SOURCE_DIR}/src/game/storage/test
    )
    target_link_libraries(chunk_store_repository_test PRIVATE
        gtnh_game_storage
        gtnh_wire
        gtnh-net
        spdlog::spdlog
        flatbuffers::flatbuffers
        asio::asio
        uring
    )
    add_dependencies(chunk_store_repository_test simcored_fbs)
    add_test(NAME chunk_store_repository_test COMMAND chunk_store_repository_test)
endif()

# inventory_action_handler_test — unit tests for InventoryActionHandler, the
# server-authoritative inventory mutation path (issue gp-b25u, parent change
# refactor-server-authoritative-inventory-verification). Same project
# CHECK/TEST harness; no GoogleTest (see the note above).
#
# The central assertion is the parent change's bug class: a REJECTED action
# must leave state COMPLETELY unchanged. Every rejection path is checked
# against a full before/after snapshot of all 40 player slots, the cursor and
# the open container — not just the one slot that was expected to be touched.
#
# Self-contained and deterministic: the router client is constructed but never
# connected (Publish drops), the EntityStateStoreClient behind
# ChestStateManager is null (saveSlots writes its cache and returns before any
# socket), and no test sleeps or polls. EntityStateStoreClient and
# IoUringRouterClient are compiled from source rather than linked against the
# simcored static lib, which would drag in main.cpp.o and a second main().
if(BUILD_TESTING)
    # gtnh_engine_sim links yaml-cpp PRIVATE (MachineRegistry.cpp), so the
    # yaml-cpp symbols the linker needs must be findable from THIS directory
    # too. Same find_package the other game subdirs use.
    find_package(yaml-cpp REQUIRED)

    add_executable(inventory_action_handler_test
        test/test_inventory_action_handler.cpp
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/clients/EntityStateStoreClient.cpp
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/clients/IoUringRouterClient.cpp
    )
    target_include_directories(inventory_action_handler_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(inventory_action_handler_test PRIVATE
        gtnh_game_storage
        gtnh_game_crafting
        gtnh_engine_sim
        gtnh_engine_registry
        gtnh-net
        EnTT::EnTT
        spdlog::spdlog
        flatbuffers::flatbuffers
        asio::asio
        yaml-cpp::yaml-cpp
        uring
        pthread
    )
    add_dependencies(inventory_action_handler_test simcored_fbs)
    add_test(NAME inventory_action_handler_test COMMAND inventory_action_handler_test)
endif()
