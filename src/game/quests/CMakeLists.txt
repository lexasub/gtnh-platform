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

    # ------------------------------------------------------------------------
    # quest_types_test — QuestTypes enum round-trip and defaults (gp-kd4u).
    # Pure header logic: no files, no clock, no network. Includes the generated
    # quest.fbs header, so it depends on this directory's own quest_lib_fbs step
    # rather than on simcore's (the two core_generated.h files are byte-identical
    # today, but each generated header is included by basename so only the
    # include path decides which one wins).
    # ------------------------------------------------------------------------
    add_executable(quest_types_test
        test/test_questtypes.cpp
    )
    target_include_directories(quest_types_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/engine/net
        ${CMAKE_SOURCE_DIR}/src/game/quests
        ${FBS_GEN_DIR}
    )
    target_link_libraries(quest_types_test PRIVATE
        spdlog::spdlog
        flatbuffers::flatbuffers
    )
    add_dependencies(quest_types_test quest_lib_fbs)
    add_test(NAME quest_types_test COMMAND quest_types_test)

    # ------------------------------------------------------------------------
    # quest_graph_test — QuestData graph construction + QuestGraph evaluation
    # (gp-9l8f). Fully synthetic temp CSV/JSON fixtures, so the shipped
    # src/content/data/quests/ dataset is never read and no test can drift with
    # the quest data.
    # ------------------------------------------------------------------------
    add_executable(quest_graph_test
        test/test_questgraph.cpp
    )
    target_include_directories(quest_graph_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/engine/net
    )
    target_link_libraries(quest_graph_test PRIVATE
        gtnh_game_quests
        spdlog::spdlog
        nlohmann_json::nlohmann_json
    )
    add_test(NAME quest_graph_test COMMAND quest_graph_test)

    # ------------------------------------------------------------------------
    # quest_manager_test — QuestManager completion, progress publication, reward
    # issuance and prerequisite/unlock gating (gp-phl3 + gp-qnrk). Synthetic
    # quest fixtures; DATA_DIR supplies the real items.csv, because the
    # detection handlers resolve packed item ids through the item registry.
    # QuestManager.cpp is compiled into gtnh_game_quests and needs
    # PlayerInventoryStore.h (for PersistSlot), so this target links the
    # storage lib and waits on simcore's FlatBuffers step too.
    # ------------------------------------------------------------------------
    add_executable(quest_manager_test
        test/test_questmanager_gating.cpp
    )
    target_compile_definitions(quest_manager_test PRIVATE
        DATA_DIR="${CMAKE_SOURCE_DIR}/src/content/data"
    )
    target_include_directories(quest_manager_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/engine/net
        ${CMAKE_SOURCE_DIR}/src/game/quests
        ${FBS_GEN_DIR}
    )
    target_link_libraries(quest_manager_test PRIVATE
        gtnh_game_quests
        gtnh_game_storage
        gtnh_engine_sim
        gtnh_engine_registry
        gtnh-net
        spdlog::spdlog
        flatbuffers::flatbuffers
        nlohmann_json::nlohmann_json
        asio::asio
        EnTT::EnTT
    )
    add_dependencies(quest_manager_test simcored_fbs)
    add_test(NAME quest_manager_test COMMAND quest_manager_test)
endif()
