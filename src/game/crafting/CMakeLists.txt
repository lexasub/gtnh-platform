# Crafting rules — grid matching, workbench state, request/complete flow.
# Extracted from simcore (game rules depend on engine, not vice versa).
add_library(gtnh_game_crafting STATIC
    GridPatternMatcher.cpp
    WorkbenchStateManager.cpp
    CraftRequestHandler.cpp
    RecipeCompletedHandler.cpp
)

target_include_directories(gtnh_game_crafting PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src/apps/simcore/simcored_fbs
)

add_dependencies(gtnh_game_crafting simcored_fbs)

target_link_libraries(gtnh_game_crafting PUBLIC
    gtnh_game_recipes
    gtnh_game_quests
    gtnh_engine_storage
    flatbuffers::flatbuffers
    EnTT::EnTT
)

target_link_libraries(gtnh_game_crafting PRIVATE
    spdlog::spdlog
    asio::asio
    gtnh-net
)

# grid_pattern_matcher_test — unit tests for GridPatternMatcher, the 3x3
# crafting-grid matcher. Self-contained: the matcher is pure array logic over
# RecipeManager::ItemStack, so the test links gtnh_game_crafting and needs no
# services, no data files, no network and no wall clock.
#
# Uses the PROJECT's own CHECK/TEST harness (src/engine/net/test/test.h,
# mirrored by src/game/machines/test/test_explosion_system.cpp and
# src/game/storage/test/test_chest_state_manager.cpp). GoogleTest is
# deliberately NOT used: gtest is absent from conanfile.txt, CI does not
# install libgtest-dev, and CI builds Release with a global -Werror — a
# find_package(GTest QUIET) guard would make the test silently stop
# registering on CI, which is worse than no test at all.
if(BUILD_TESTING)
    add_executable(grid_pattern_matcher_test
        test/test_grid_pattern_matcher.cpp
    )
    target_include_directories(grid_pattern_matcher_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(grid_pattern_matcher_test PRIVATE
        gtnh_game_crafting
        gtnh_game_recipes
    )
    add_test(NAME grid_pattern_matcher_test COMMAND grid_pattern_matcher_test)
endif()

# crafting_state_machine_test — one target for the three workbench state-machine
# files: CraftRequestHandler, RecipeCompletedHandler and WorkbenchStateManager.
# They are deliberately ONE target rather than three: they share the same
# fixtures (a recipe manager, an inventory store, a workbench cache and a
# recording publisher), and three separate targets would each rebuild and
# re-create those fixtures independently while racing on the same static
# ItemRegistry singleton. GridPatternMatcher above is separate precisely because
# it is standalone pure array logic.
#
# Same harness as grid_pattern_matcher_test above. The simcore clients are
# compiled from source rather than linked from the simcored static lib, which
# would drag in main.cpp.o and a second main(). Both are pure socket wrappers
# and are driven UNCONNECTED here, so no socket, thread or network is created.
if(BUILD_TESTING)
    find_package(yaml-cpp REQUIRED)

    add_executable(crafting_state_machine_test
        test/test_crafting_state_machine.cpp
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/clients/EntityStateStoreClient.cpp
        ${CMAKE_SOURCE_DIR}/src/apps/simcore/Network/clients/IoUringRouterClient.cpp
    )
    target_compile_definitions(crafting_state_machine_test PRIVATE
        DATA_DIR="${CMAKE_SOURCE_DIR}/src/content/data"
    )
    target_include_directories(crafting_state_machine_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/apps/simcore
    )
    target_link_libraries(crafting_state_machine_test PRIVATE
        gtnh_game_crafting
        gtnh_game_storage
        gtnh_game_recipes
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
    add_dependencies(crafting_state_machine_test simcored_fbs)
    add_test(NAME crafting_state_machine_test COMMAND crafting_state_machine_test)
endif()
