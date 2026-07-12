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
    COMMAND flatbuffers::flatc
        --gen-mutable
        -I ${FBS_DIR}
        -o ${FBS_GEN_DIR}
        -c ${FBS_DIR}/core.fbs ${FBS_DIR}/simcore.fbs ${FBS_DIR}/chunkstore.fbs ${FBS_DIR}/recipe.fbs ${FBS_DIR}/meta_db.fbs ${FBS_DIR}/machine_state.fbs ${FBS_DIR}/entity_state_store.fbs ${FBS_DIR}/pipe_network.fbs ${FBS_DIR}/quest.fbs
    DEPENDS flatbuffers::flatc
            ${FBS_DIR}/core.fbs
            ${FBS_DIR}/simcore.fbs
            ${FBS_DIR}/chunkstore.fbs
            ${FBS_DIR}/recipe.fbs
            ${FBS_DIR}/meta_db.fbs
            ${FBS_DIR}/machine_state.fbs
            ${FBS_DIR}/pipe_network.fbs
            ${FBS_DIR}/quest.fbs
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
)

# C++ library
add_library(simcored
        main.cpp
        Quest/QuestManager.cpp

        Network/RouterEventPublisher.cpp
        Network/PipeEnergyClient.cpp
        Network/FluidClient.cpp
        Network/ItemClient.cpp
        Network/TopicDispatcher.cpp
        Network/clients/EntityStateStoreClient.cpp
        Network/clients/IoUringRouterClient.cpp
        Network/clients/IoUringChunkClient.cpp

        Storage/ChunkStoreRepository.cpp
        Storage/PlayerInventoryStore.cpp
        Storage/InventoryLoadHandler.cpp
        Storage/InventoryActionHandler.cpp
        Storage/PlayerJoinedHandler.cpp

        Actions/ActionDispatcher.cpp
        Actions/SetBlockCASHandler.cpp
        Actions/WrenchHandler.cpp
        Actions/ElectricDrillHandler.cpp
        Actions/MachineSlotHandler.cpp
        Actions/ToolActionHandler.cpp

        World/ChunkEventHandler.cpp
        World/BlockTransforms.cpp
        World/ChunkSnapshot.cpp
        World/WorldContainerInventory.cpp

        Crafting/CraftRequestHandler.cpp
        Crafting/RecipeCompletedHandler.cpp
        ECS/SimulationEngine.cpp

        ECS/Reactors/EnergyFlowHandler.cpp
        ECS/Reactors/FluidFlowHandler.cpp
        ECS/Reactors/ItemFlowHandler.cpp

        ECS/Systems/HeatTransferSystem.cpp
        ECS/Systems/GeneratorSystem.cpp
        ECS/Systems/CreativeGeneratorSystem.cpp
        ECS/Systems/MachineSystem.cpp
        ECS/Systems/BoilerSystem.cpp
        ECS/Systems/BatteryBufferSystem.cpp
        ECS/Systems/DrillSystem.cpp
        ECS/Systems/ExplosionSystem.cpp
        ECS/Systems/TransformerSystem.cpp
        ECS/Systems/RotareGeneratorSystem.cpp

        RecipeManager/RecipeManager.cpp
)

target_include_directories(simcored PUBLIC
    ${CMAKE_SOURCE_DIR}/src/libs
    Network/clients/
    ${CMAKE_SOURCE_DIR}/src
PRIVATE
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
    .
)

add_subdirectory(Crafting)

add_dependencies(simcored simcored_fbs)

target_link_libraries(simcored PUBLIC
    Crafting
    gtnh-net
PRIVATE
    machine_registry
    recipe_manager_lib
    quest_lib
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
    simcored
    machine_registry
    asio::asio
    flatbuffers::flatbuffers
    recipe_manager_lib
)

# ---------------------------------------------------------------------------
# Tests — struct layout + ECS systems
# ---------------------------------------------------------------------------
add_executable(simcored_test
    test/test_main.cpp
    test/test_ecs_systems.cpp
    test/test_recipe_manager.cpp
)
target_compile_definitions(simcored_test PRIVATE
    DATA_DIR="${CMAKE_SOURCE_DIR}/data"
)
target_include_directories(simcored_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libs
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
    .
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(simcored_test PRIVATE
    simcored
    gtnh-net
    machine_registry
    recipe_manager_lib
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

# Install
install(TARGETS simcored simcored_exec simcored_test
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)
