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
