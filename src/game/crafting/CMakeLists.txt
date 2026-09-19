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
