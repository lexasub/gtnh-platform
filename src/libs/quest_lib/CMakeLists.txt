# Quest library — data model + graph only (shared by game_client + simcore)
# QuestCompletionDetector + QuestManager live directly in simulation_core
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(quest_lib STATIC
    QuestTypes.cpp
    QuestData.cpp
    QuestGraph.cpp
)

target_include_directories(quest_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(quest_lib PUBLIC
    flatbuffers::flatbuffers
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
