cmake_minimum_required(VERSION 3.20)

project(simcored LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# EnTT
find_package(EnTT REQUIRED)

# FlatBuffers
find_package(flatbuffers REQUIRED)

# Math / Logging
find_package(glm REQUIRED)
find_package(spdlog REQUIRED)

find_package(SQLite3 REQUIRED)

# FlatBuffers schema → C++ headers — shared protocol from src/protocol/
set(FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
set(FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/simcored_fbs)
file(MAKE_DIRECTORY ${FBS_GEN_DIR})

add_custom_command(
    OUTPUT
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/simcore_generated.h
        ${FBS_GEN_DIR}/chunkstore_generated.h
        ${FBS_GEN_DIR}/recipe_generated.h
        ${FBS_GEN_DIR}/meta_db_generated.h
        ${FBS_GEN_DIR}/machine_state_generated.h
        ${FBS_GEN_DIR}/entity_state_store_generated.h
        ${FBS_GEN_DIR}/pipe_network_generated.h
        ${FBS_GEN_DIR}/quest_generated.h
        ${FBS_GEN_DIR}/multiblock_state_generated.h
        ${FBS_GEN_DIR}/client_state_generated.h
    COMMAND flatbuffers::flatc
        --gen-mutable
        -I ${FBS_DIR}
        -o ${FBS_GEN_DIR}
        -c ${FBS_DIR}/core.fbs ${FBS_DIR}/simcore.fbs ${FBS_DIR}/chunkstore.fbs ${FBS_DIR}/recipe.fbs ${FBS_DIR}/meta_db.fbs ${FBS_DIR}/machine_state.fbs ${FBS_DIR}/entity_state_store.fbs ${FBS_DIR}/pipe_network.fbs ${FBS_DIR}/quest.fbs ${FBS_DIR}/multiblock_state.fbs ${FBS_DIR}/client_state.fbs
    DEPENDS flatbuffers::flatc
            ${FBS_DIR}/core.fbs
            ${FBS_DIR}/simcore.fbs
            ${FBS_DIR}/chunkstore.fbs
            ${FBS_DIR}/recipe.fbs
            ${FBS_DIR}/meta_db.fbs
            ${FBS_DIR}/machine_state.fbs
            ${FBS_DIR}/pipe_network.fbs
            ${FBS_DIR}/quest.fbs
            ${FBS_DIR}/multiblock_state.fbs
            ${FBS_DIR}/client_state.fbs
    COMMENT "Generating FlatBuffers headers for simulation_core"
)

add_custom_target(simcored_fbs
    DEPENDS
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/simcore_generated.h
        ${FBS_GEN_DIR}/recipe_generated.h
        ${FBS_GEN_DIR}/meta_db_generated.h
        ${FBS_GEN_DIR}/machine_state_generated.h
        ${FBS_GEN_DIR}/pipe_network_generated.h
        ${FBS_GEN_DIR}/quest_generated.h
        ${FBS_GEN_DIR}/multiblock_state_generated.h
        ${FBS_GEN_DIR}/client_state_generated.h
)

# gtnh_engine_sim compiles SimulationEngine.cpp which includes
# multiblock_state_generated.h from the simcore fbs step.
if(TARGET gtnh_engine_sim)
    add_dependencies(gtnh_engine_sim simcored_fbs)
endif()

# C++ library
add_library(simcored
        main.cpp

        Network/RouterEventPublisher.cpp
        Network/PipeEnergyClient.cpp
        Network/FluidClient.cpp
        Network/ItemClient.cpp
        Network/ResourceBufferStatePublisher.cpp
        Network/TopicDispatcher.cpp
        Network/SimCoreMessageHandler.cpp
        Network/ResourceDrainHandler.cpp
        Network/CraftReservationClient.cpp
        Network/WorkbenchOpenHandler.cpp
        Network/ChestOpenHandler.cpp
        Network/ChestCloseHandler.cpp
        Network/MachineOpenHandler.cpp
        Network/MachineCloseHandler.cpp
        Network/clients/EntityStateStoreClient.cpp
        Network/clients/IoUringRouterClient.cpp
        Network/clients/IoUringChunkClient.cpp


        ECS/Reactors/EnergyFlowHandler.cpp

        ECS/Reactors/FluidFlowHandler.cpp
        ECS/Reactors/ItemFlowHandler.cpp
        ECS/Reactors/CableExplosionHandler.cpp


        RecipeManager/RecipeManager.cpp
)

target_include_directories(simcored PUBLIC
    Network/clients/
PRIVATE
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
    .
)


add_dependencies(simcored simcored_fbs)

target_link_libraries(simcored PUBLIC
    gtnh_game_storage
    gtnh_game_scenario
    gtnh_game_world
    gtnh_game_actions
    gtnh_game_machines
    gtnh_game_mining
    gtnh_game_recipes
    gtnh_game_quests
    gtnh-net
PRIVATE
    gtnh_engine_sim
    gtnh_game_mining
    gtnh_game_recipes
    gtnh_game_quests
    gtnh_content
    asio::asio
    flatbuffers::flatbuffers
    EnTT::EnTT
    lmdb::lmdb
    spdlog::spdlog
    SQLite::SQLite3
    pthread
)

# Executable
add_executable(simcored_exec
    main.cpp
)

target_include_directories(simcored_exec PRIVATE
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src/libs
    .
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(simcored_exec PRIVATE
    # Circular static deps: game/machines calls back into simcored
    # (Network clients, RecipeManager) while simcored calls machine systems.
    "$<LINK_GROUP:RESCAN,gtnh_engine_sim,simcored,gtnh_game_machines,gtnh_game_mining,gtnh_game_recipes,gtnh_game_quests,gtnh_game_crafting,gtnh_game_world,gtnh_game_actions,gtnh_game_storage,gtnh_game_scenario>"
    gtnh-net
    asio::asio
    flatbuffers::flatbuffers
)

# ---------------------------------------------------------------------------
# Tests — struct layout + ECS systems
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
    add_executable(simcored_test
        test/test_main.cpp
        test/test_ecs_systems.cpp
        test/test_recipe_manager.cpp
        test/test_quest_manager.cpp
        test/test_game_scenario.cpp
        test/test_inventory_click.cpp
        test/test_container_click.cpp
        test/test_wrench_meta.cpp
        test/test_boiler_ports.cpp
        test/test_resource_drain.cpp
        test/test_pending_craft.cpp
    )
    target_compile_definitions(simcored_test PRIVATE
        DATA_DIR="${CMAKE_SOURCE_DIR}/src/content/data"
    )
    target_include_directories(simcored_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src/libs
        ${FBS_GEN_DIR}
        ${flatbuffers_INCLUDE_DIRS}
        .
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(simcored_test PRIVATE
        "$<LINK_GROUP:RESCAN,gtnh_engine_sim,simcored,gtnh_game_machines,gtnh_game_mining,gtnh_game_recipes,gtnh_game_quests,gtnh_game_crafting,gtnh_game_world,gtnh_game_actions,gtnh_game_storage,gtnh_game_scenario,gtnh-net>"
        asio::asio
        flatbuffers::flatbuffers
        EnTT::EnTT
        lmdb::lmdb
        spdlog::spdlog
        SQLite::SQLite3
        pthread
    )
    add_test(NAME simcored_test COMMAND simcored_test)
    add_dependencies(simcored_test simcored_fbs)
endif()

# Install
install(TARGETS simcored simcored_exec
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)
if(BUILD_TESTING)
    install(TARGETS simcored_test RUNTIME DESTINATION bin)
endif()
