# FlatBuffers schema → C++ headers — shared protocol from src/protocol/
set(FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
set(FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/chunkd_fbs)
file(MAKE_DIRECTORY ${FBS_GEN_DIR})

set(CMAKE_CXX_STANDARD 26)

find_package(Threads REQUIRED)

# For common/coords/Coords.h
include_directories(${CMAKE_SOURCE_DIR}/src)

add_custom_command(
    OUTPUT
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/chunkstore_generated.h
    COMMAND flatbuffers::flatc
        --gen-mutable
        -I ${FBS_DIR}
        -o ${FBS_GEN_DIR}
        -c ${FBS_DIR}/core.fbs ${FBS_DIR}/chunkstore.fbs
    DEPENDS flatbuffers::flatc
            ${FBS_DIR}/core.fbs
            ${FBS_DIR}/chunkstore.fbs
    COMMENT "Generating FlatBuffers headers for chunk_store"
)

add_custom_target(chunkd_fbs
    DEPENDS
        ${FBS_GEN_DIR}/core_generated.h
        ${FBS_GEN_DIR}/chunkstore_generated.h
)

add_executable(chunkd
    Network/ChunkStoreService.cpp
    Network/RouterClient.cpp

    Storage/ChunkStore.cpp
    Storage/WorkQueue.cpp
    World/ServerWorld.cpp
    main.cpp
)

# vcpkg's unofficial-lmdb transitively adds its entire include/ dir, which contains a newer
# flatbuffers.h (25.12) incompatible with our Conan flatc (25.9). Ensure Conan's version wins.
include_directories(BEFORE SYSTEM ${flatbuffers_INCLUDE_DIRS} ${FBS_GEN_DIR})

add_dependencies(chunkd chunkd_fbs)

# Lock-free ClockCache test — header-only, no external deps beyond Threads
add_executable(clock_cache_test
    Storage/clock_cache_test.cpp
)
target_compile_options(clock_cache_test PRIVATE -mcx16)
target_link_libraries(clock_cache_test PRIVATE Threads::Threads)

# ChunkStore + ClockCache integration test — links to real ChunkStore + LMDB
add_executable(chunk_cache_test
    Storage/ChunkStore.cpp
    Storage/WorkQueue.cpp
    Storage/chunk_cache_test.cpp
)
target_compile_options(chunk_cache_test PRIVATE -mcx16)
add_dependencies(chunk_cache_test chunkd_fbs)

target_link_libraries(chunk_cache_test PRIVATE
    unofficial::lmdb::lmdb
    spdlog::spdlog
    Threads::Threads
    worldgeneratord
)

# ClockCache vs LruCache microbenchmark — header-only, Threads only
add_executable(clock_cache_bench
    Storage/clock_cache_bench.cpp
)
target_compile_options(clock_cache_bench PRIVATE -mcx16)
target_link_libraries(clock_cache_bench PRIVATE Threads::Threads)

target_link_libraries(chunkd PRIVATE
    unofficial::lmdb::lmdb
    flatbuffers::flatbuffers
    glm::glm
    spdlog::spdlog
    asio::asio
    Threads::Threads
    worldgeneratord
)

add_executable(chunk_store_cli_test
    chunk_store_cli_test.cpp
    Storage/ChunkStore.cpp
    Storage/WorkQueue.cpp
)
add_dependencies(chunk_store_cli_test chunkd_fbs)
target_compile_options(chunk_store_cli_test PRIVATE -mcx16)
target_link_libraries(chunk_store_cli_test PRIVATE
    unofficial::lmdb::lmdb
    spdlog::spdlog
    Threads::Threads
    worldgeneratord
)

# Load test — throughput, latency, mixed read/write, eviction stress
add_executable(chunkd_load_test
    Storage/chunkd_load_test.cpp
    Storage/ChunkStore.cpp
    Storage/WorkQueue.cpp
)
add_dependencies(chunkd_load_test chunkd_fbs)
target_compile_options(chunkd_load_test PRIVATE -mcx16)
target_link_libraries(chunkd_load_test PRIVATE
    unofficial::lmdb::lmdb
    spdlog::spdlog
    Threads::Threads
    worldgeneratord
)

# Dirty-chunk tracking test — real LMDB, real concurrency
add_executable(chunk_dirty_test
    Storage/chunk_dirty_test.cpp
    Storage/ChunkStore.cpp
    Storage/WorkQueue.cpp
)
add_dependencies(chunk_dirty_test chunkd_fbs)
target_compile_options(chunk_dirty_test PRIVATE -mcx16)
target_link_libraries(chunk_dirty_test PRIVATE
    unofficial::lmdb::lmdb
    spdlog::spdlog
    Threads::Threads
    worldgeneratord
)
