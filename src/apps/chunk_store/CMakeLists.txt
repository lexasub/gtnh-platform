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

# Lightweight codec/cache objects shared with pipe_network — no LMDB, no
# world_generator dependency (chunkd_storage keeps the full storage stack).
add_library(chunkd_codec
    Storage/cache/MutableChunk.cpp
    Storage/cache/MutableSection.cpp
    Storage/SectionCodec.cpp
)

target_include_directories(chunkd_codec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(chunkd_codec PUBLIC
    spdlog::spdlog
)

add_library(chunkd_storage
    Storage/disk/LmdbStore.cpp
    Storage/disk/FlushPipeline.cpp
    Storage/ChunkStore.cpp
    Storage/EncodePipeline.cpp
    Storage/CASHandler.cpp
    Storage/WorkQueue.cpp
)

target_link_libraries(chunkd_storage
    PRIVATE
    asio::asio
    worldgeneratord # TODO - may be remove deps
    PUBLIC
    chunkd_codec
    lmdb::lmdb
    spdlog::spdlog
    atomic
)

add_executable(chunkd
    Network/IoUringChunkStoreService.cpp
    Network/IoUringRouterClient.cpp

    main.cpp
)

# lmdb transitively adds its entire include/ dir, which contains a newer
# flatbuffers.h (25.12) incompatible with our Conan flatc (25.9). Ensure Conan's version wins.
include_directories(BEFORE SYSTEM ${flatbuffers_INCLUDE_DIRS} ${FBS_GEN_DIR})

add_dependencies(chunkd chunkd_fbs asio::asio chunkd_storage gtnh-net)

# Lock-free ClockCache test — header-only, no external deps beyond Threads
if(BUILD_TESTING)
    add_executable(clock_cache_test
        Storage/clock_cache_test.cpp
    )
    target_compile_options(clock_cache_test PRIVATE -mcx16)
    target_link_libraries(clock_cache_test PRIVATE Threads::Threads chunkd_storage)

    # ChunkStore + ClockCache integration test — links to real ChunkStore + LMDB
    add_executable(chunk_cache_test
        Storage/chunk_cache_test.cpp
    )
    target_compile_options(chunk_cache_test PRIVATE -mcx16)
    add_dependencies(chunk_cache_test chunkd_fbs)

    target_link_libraries(chunk_cache_test PRIVATE
        lmdb::lmdb
        spdlog::spdlog
        Threads::Threads
        worldgeneratord
        asio::asio
        chunkd_storage
    )
endif()

# ClockCache vs LruCache microbenchmark — header-only, Threads only
add_executable(clock_cache_bench
    Storage/clock_cache_bench.cpp
)
target_compile_options(clock_cache_bench PRIVATE -mcx16)
target_link_libraries(clock_cache_bench PRIVATE Threads::Threads atomic)

target_link_libraries(chunkd PRIVATE
    lmdb::lmdb
    flatbuffers::flatbuffers
    glm::glm
    spdlog::spdlog
    asio::asio
    gtnh-net
    uring
    Threads::Threads
    worldgeneratord
    chunkd_storage
)

if(BUILD_TESTING)
    add_executable(chunk_store_cli_test
        chunk_store_cli_test.cpp
    )

    add_dependencies(chunk_store_cli_test chunkd_fbs chunkd_storage)
    target_compile_options(chunk_store_cli_test PRIVATE -mcx16)
    target_link_libraries(chunk_store_cli_test PRIVATE
        lmdb::lmdb
        spdlog::spdlog
        Threads::Threads
        worldgeneratord
        asio::asio
        chunkd_storage
    )

    # Load test — throughput, latency, mixed read/write, eviction stress
    add_executable(chunkd_load_test
        Storage/chunkd_load_test.cpp
    )
    add_dependencies(chunkd_load_test chunkd_fbs)
    target_compile_options(chunkd_load_test PRIVATE -mcx16)
    target_link_libraries(chunkd_load_test PRIVATE
        lmdb::lmdb
        spdlog::spdlog
        Threads::Threads
        worldgeneratord
        asio::asio
        chunkd_storage
    )

    # LMDB commit-failure test (gp-q4aa) — a failed mdb_txn_commit must be
    # reported to the caller, not swallowed. Uses the project CHECK/TEST macro
    # harness; there is deliberately no find_package(GTest) guard, because a
    # guarded test silently vanishes from CI instead of failing it.
    add_executable(lmdb_commit_test
        Storage/lmdb_commit_test.cpp
    )
    add_dependencies(lmdb_commit_test chunkd_fbs asio::asio)
    target_compile_options(lmdb_commit_test PRIVATE -mcx16)
    target_link_libraries(lmdb_commit_test PRIVATE
        lmdb::lmdb
        spdlog::spdlog
        Threads::Threads
        worldgeneratord
        asio::asio
        chunkd_storage
    )
    add_test(NAME lmdb_commit_test COMMAND lmdb_commit_test)

    # LMDB open-path test (gp-6nmw) — a failed mdb_env_open must be reported by
    # the constructor, not leave a store that looks usable. Exercises a real
    # unopenable path (missing parent dir) inside a forked child, because the
    # pre-fix behaviour is a SIGSEGV inside LMDB rather than a clean false.
    add_executable(lmdb_open_test
        Storage/lmdb_open_test.cpp
    )
    add_dependencies(lmdb_open_test chunkd_fbs asio::asio)
    target_compile_options(lmdb_open_test PRIVATE -mcx16)
    target_link_libraries(lmdb_open_test PRIVATE
        lmdb::lmdb
        spdlog::spdlog
        Threads::Threads
        worldgeneratord
        asio::asio
        chunkd_storage
    )
    add_test(NAME lmdb_open_test COMMAND lmdb_open_test)

    # LMDB batch-write test (gp-jzgr) — writeBatch must handle MDB_MAP_FULL
    # with a grow+retry (as writeRaw already did), must not lose the whole
    # batch when one entry does not fit, and must leave the caller's entries
    # intact on failure so a caller cannot drop them.
    add_executable(lmdb_batch_test
        Storage/lmdb_batch_test.cpp
    )
    add_dependencies(lmdb_batch_test chunkd_fbs asio::asio)
    target_compile_options(lmdb_batch_test PRIVATE -mcx16)
    target_link_libraries(lmdb_batch_test PRIVATE
        lmdb::lmdb
        spdlog::spdlog
        Threads::Threads
        worldgeneratord
        asio::asio
        chunkd_storage
    )
    add_test(NAME lmdb_batch_test COMMAND lmdb_batch_test)

    # ChunkStore read-failure test (gp-wjmb) — a read that cannot be performed
    # must not be presented as "chunk absent". Induces a genuine LMDB read
    # error (MDB_READERS_FULL) by holding the shared reader table full from a
    # second env, and asserts the stored chunk is not overwritten.
    add_executable(lmdb_read_test
        Storage/lmdb_read_test.cpp
    )
    add_dependencies(lmdb_read_test chunkd_fbs asio::asio)
    target_compile_options(lmdb_read_test PRIVATE -mcx16)
    target_link_libraries(lmdb_read_test PRIVATE
        lmdb::lmdb
        spdlog::spdlog
        Threads::Threads
        worldgeneratord
        asio::asio
        chunkd_storage
    )
    add_test(NAME lmdb_read_test COMMAND lmdb_read_test)

    # Daemon-startup contract test (gp-6nmw, second half) — drives the real
    # chunkd binary and asserts an unopenable db is a startup failure, not a
    # daemon that announces a listener over a dead store. Depends on `chunkd`
    # and receives its path as argv[1].
    add_executable(lmdb_daemon_start_test
        Storage/lmdb_daemon_start_test.cpp
    )
    add_dependencies(lmdb_daemon_start_test chunkd chunkd_fbs asio::asio)
    target_compile_options(lmdb_daemon_start_test PRIVATE -mcx16)
    target_link_libraries(lmdb_daemon_start_test PRIVATE Threads::Threads)
    add_test(NAME lmdb_daemon_start_test
             COMMAND lmdb_daemon_start_test $<TARGET_FILE:chunkd>)

    # Dirty-chunk tracking test — real LMDB, real concurrency
    add_executable(chunk_dirty_test
        Storage/chunk_dirty_test.cpp
    )
    add_dependencies(chunk_dirty_test chunkd_fbs asio::asio)
    target_compile_options(chunk_dirty_test PRIVATE -mcx16)
    target_link_libraries(chunk_dirty_test PRIVATE
        lmdb::lmdb
        spdlog::spdlog
        Threads::Threads
        worldgeneratord
        asio::asio
        chunkd_storage
    )
    # gp-8e7: this target was built but never registered, so ctest never ran
    # it. Every case uses a mkdtemp LMDB directory and removes it afterwards.
    add_test(NAME chunk_dirty_test COMMAND chunk_dirty_test)
    set_tests_properties(chunk_dirty_test PROPERTIES TIMEOUT 300)
endif()
