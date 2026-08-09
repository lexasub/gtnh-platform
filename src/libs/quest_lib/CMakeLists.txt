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

# quest_lib unit tests — data loaders only (no network/ECS dependencies).
add_executable(quest_lib_test
    test/test_questdata.cpp
)
target_compile_definitions(quest_lib_test PRIVATE
    DATA_DIR="${CMAKE_SOURCE_DIR}/data"
)
target_include_directories(quest_lib_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libs
    ${CMAKE_SOURCE_DIR}/src/libs/libgtnh-net
)
target_link_libraries(quest_lib_test PRIVATE
    quest_lib
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)
add_test(NAME quest_lib_test COMMAND quest_lib_test)
