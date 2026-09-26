add_library(gtnh_game_world STATIC
  BlockDrops.cpp
  BlockTransforms.cpp
  ChunkEventHandler.cpp
  ChunkSnapshot.cpp
  WorldContainerInventory.cpp
)
target_include_directories(gtnh_game_world PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/apps/simcore
  ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
)
add_dependencies(gtnh_game_world simcored_fbs)
target_link_libraries(gtnh_game_world PUBLIC
  gtnh_engine_registry
  gtnh_wire
  gtnh-net
  EnTT::EnTT
  flatbuffers::flatbuffers
  spdlog::spdlog
)

target_link_libraries(gtnh_game_world PRIVATE
  asio::asio
)

# block_transforms_test — unit tests for the CSV-driven "placed X on Y -> Z"
# rule table. Uses the project's own CHECK/TEST harness (see
# src/engine/net/test/test.h and src/game/quests/test/test_questdata.cpp); the
# project has no GoogleTest dependency and CI does not install it.
if(BUILD_TESTING)
    add_executable(block_transforms_test
        test/BlockTransforms_test.cpp
    )
    target_compile_definitions(block_transforms_test PRIVATE
        REGISTRY_TRANSFORMS_DIR="${CMAKE_SOURCE_DIR}/src/content/data/registry"
    )
    target_include_directories(block_transforms_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(block_transforms_test PRIVATE
        gtnh_game_world
    )
    add_test(NAME block_transforms_test COMMAND block_transforms_test)
endif()

# chunk_event_test — unit tests for ChunkSnapshot.cpp (snapshot construction)
# and ChunkEventHandler.cpp (event validation and dispatch order). Uses the
# project's own CHECK/TEST harness; the repo has no GoogleTest dependency and
# CI does not install it, so there is deliberately no find_package(GTest)
# guard — a guarded test silently vanishes from CI instead of failing it.
# The handler dispatches into SimulationEngine, so this target links
# gtnh_engine_sim plus the EnTT/flatbuffers headers the ECS views and the
# test's own event builder use.
if(BUILD_TESTING)
    add_executable(chunk_event_test
        test/ChunkEvent_test.cpp
    )
    target_include_directories(chunk_event_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(chunk_event_test PRIVATE
        gtnh_game_world
        gtnh_engine_sim
        EnTT::EnTT
        flatbuffers::flatbuffers
        spdlog::spdlog
    )
    add_test(NAME chunk_event_test COMMAND chunk_event_test)
endif()

# block_drops_test — unit tests for BlockDrops.cpp, the CSV-driven
# "broken X yields Y" rule table. Same harness and same rationale as
# block_transforms_test above: the project has no GoogleTest dependency and CI
# does not install it, so there is deliberately no find_package(GTest) guard
# (a guarded test silently vanishes from CI instead of failing it). One test
# pins the shipped src/content/data/registry/drops.csv, hence the
# REGISTRY_DROPS_DIR define; every other test writes its own temp fixture.
if(BUILD_TESTING)
    add_executable(block_drops_test
        test/BlockDrops_test.cpp
    )
    target_compile_definitions(block_drops_test PRIVATE
        REGISTRY_DROPS_DIR="${CMAKE_SOURCE_DIR}/src/content/data/registry"
    )
    target_include_directories(block_drops_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(block_drops_test PRIVATE
        gtnh_game_world
    )
    add_test(NAME block_drops_test COMMAND block_drops_test)
endif()

# world_container_inventory_test — unit tests for WorldContainerInventory.cpp
# (container open/close/action, the flatbuffers blob round trip and the ECS
# entity lifecycle). Same harness and same no-GoogleTest rationale as the
# targets above.
#
# The class takes a std::shared_ptr to the CONCRETE, non-virtual
# EntityStateStoreClient, so it cannot be injected with a double. This target
# therefore compiles test/stub_entity_state_client.cpp, a link-time stub that
# DEFINES the client members WorldContainerInventory.cpp calls and invokes the
# callbacks synchronously — no socket, no io_context run, no thread, no
# wall-clock waiting. Same pattern as src/game/storage/test/stub_chunk_client.cpp.
# The real EntityStateStoreClient.cpp is deliberately NOT added here (it is
# linked into simcored only, and adding it would be a duplicate symbol).
if(BUILD_TESTING)
    add_executable(world_container_inventory_test
        test/WorldContainerInventory_test.cpp
        test/stub_entity_state_client.cpp
    )
    target_include_directories(world_container_inventory_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(world_container_inventory_test PRIVATE
        gtnh_game_world
        gtnh_engine_registry
        EnTT::EnTT
        flatbuffers::flatbuffers
        spdlog::spdlog
        asio::asio
    )
    add_dependencies(world_container_inventory_test simcored_fbs)
    add_test(NAME world_container_inventory_test COMMAND world_container_inventory_test)
endif()
