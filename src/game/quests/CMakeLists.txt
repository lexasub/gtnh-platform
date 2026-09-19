# Quest library — data model, graph + QuestManager (shared by game_client + simcore)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(flatbuffers REQUIRED)

# Own FlatBuffers step for quest.fbs — avoids depending on simcore's gen target
set(FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
set(FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/quest_fbs)
file(MAKE_DIRECTORY ${FBS_GEN_DIR})

add_custom_command(
    OUTPUT
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/quest_generated.h
    COMMAND flatbuffers::flatc
        --gen-mutable
        -I ${FBS_DIR}
        -o ${FBS_GEN_DIR}
        -c ${FBS_DIR}/core.fbs ${FBS_DIR}/quest.fbs
    DEPENDS flatbuffers::flatc
            ${FBS_DIR}/core.fbs
            ${FBS_DIR}/quest.fbs
    COMMENT "Generating FlatBuffers headers for gtnh_game_quests"
)

add_custom_target(quest_lib_fbs
    DEPENDS
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/quest_generated.h
)

add_library(gtnh_game_quests STATIC
    QuestTypes.cpp
    QuestData.cpp
    QuestGraph.cpp
    QuestManager.cpp
)

target_include_directories(gtnh_game_quests PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

# QuestManager.cpp includes quest_generated.h (own gen) and reaches into
# simcore for Storage/PlayerInventoryStore.h (PersistSlot definition).
target_include_directories(gtnh_game_quests PRIVATE
    ${FBS_GEN_DIR}
    ${CMAKE_SOURCE_DIR}/src/apps/simcore
)

add_dependencies(gtnh_game_quests quest_lib_fbs)

target_link_libraries(gtnh_game_quests PUBLIC
    gtnh_game_recipes
    flatbuffers::flatbuffers
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# quest_lib unit tests — data loaders only (no network/ECS dependencies).
if(BUILD_TESTING)
    add_executable(quest_lib_test
        test/test_questdata.cpp
    )
    target_compile_definitions(quest_lib_test PRIVATE
        DATA_DIR="${CMAKE_SOURCE_DIR}/src/content/data"
    )
    target_include_directories(quest_lib_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/engine/net
    )
    target_link_libraries(quest_lib_test PRIVATE
        gtnh_game_quests
        spdlog::spdlog
        nlohmann_json::nlohmann_json
    )
    add_test(NAME quest_lib_test COMMAND quest_lib_test)
endif()
