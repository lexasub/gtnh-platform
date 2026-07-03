cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(flatbuffers REQUIRED)
find_package(spdlog REQUIRED)
find_package(asio REQUIRED)
find_package(Threads REQUIRED)

# Source from simulation_core needed by this service
set(SIMCORE_NET_DIR ${CMAKE_SOURCE_DIR}/src/services/simulation_core/Network)

# FlatBuffers schema → C++ headers — shared protocol from src/protocol/
set(FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
set(FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/entitystated_fbs)
file(MAKE_DIRECTORY ${FBS_GEN_DIR})

add_custom_command(
    OUTPUT
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/chunkstore_generated.h
        ${FBS_GEN_DIR}/tile_entity_store_generated.h
        ${FBS_GEN_DIR}/entity_state_store_generated.h
    COMMAND flatbuffers::flatc
        --gen-mutable
        -I ${FBS_DIR}
        -o ${FBS_GEN_DIR}
        -c ${FBS_DIR}/core.fbs ${FBS_DIR}/chunkstore.fbs ${FBS_DIR}/tile_entity_store.fbs ${FBS_DIR}/entity_state_store.fbs
    DEPENDS flatbuffers::flatc
            ${FBS_DIR}/core.fbs
            ${FBS_DIR}/chunkstore.fbs
            ${FBS_DIR}/tile_entity_store.fbs
            ${FBS_DIR}/entity_state_store.fbs
    COMMENT "Generating FlatBuffers headers for entity_state_store"
)

add_custom_target(entitystated_fbs
    DEPENDS
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/chunkstore_generated.h
        ${FBS_GEN_DIR}/tile_entity_store_generated.h
        ${FBS_GEN_DIR}/entity_state_store_generated.h
)

add_executable(entitystated
    main.cpp
    cache.cpp
    EntityStateStorage.cpp
    Client/MessageRouterClient.cpp
    Client/ChunkStoreClient.cpp
)

target_include_directories(entitystated PRIVATE
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src/services
    ${CMAKE_SOURCE_DIR}/src/services/storage_interfaces
    ${CMAKE_SOURCE_DIR}/src/services/simulation_core
    ${CMAKE_SOURCE_DIR}/src/services/simulation_core/Network
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(entitystated entitystated_fbs)

target_link_libraries(entitystated PRIVATE
    flatbuffers::flatbuffers
    spdlog::spdlog
    asio::asio
    Threads::Threads
    lmdb::lmdb
)

# Unit tests
add_executable(entitystated_test
    test_persistence.cpp
    EntityStateStorage.cpp
    cache.cpp
    Client/MessageRouterClient.cpp
    Client/ChunkStoreClient.cpp
)
target_include_directories(entitystated_test PRIVATE
    ${FBS_GEN_DIR}
    ${flatbuffers_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src/services
    ${CMAKE_SOURCE_DIR}/src/services/storage_interfaces
    ${CMAKE_SOURCE_DIR}/src/services/simulation_core
    ${CMAKE_SOURCE_DIR}/src/services/simulation_core/Network
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_dependencies(entitystated_test entitystated_fbs)
target_link_libraries(entitystated_test PRIVATE
    flatbuffers::flatbuffers
    spdlog::spdlog
    asio::asio
    Threads::Threads
    lmdb::lmdb
)
add_test(NAME entitystated_test COMMAND entitystated_test)
