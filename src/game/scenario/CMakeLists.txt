add_library(gtnh_game_scenario STATIC
    GameScenario.cpp
)

target_include_directories(gtnh_game_scenario PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(gtnh_game_scenario PUBLIC
    gtnh_game_storage
    spdlog::spdlog
    flatbuffers::flatbuffers
)
