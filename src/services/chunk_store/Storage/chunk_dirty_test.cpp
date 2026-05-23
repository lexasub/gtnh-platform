// Dirty-chunk tracking tests: markDirty, flushDirtyChunks, setBlock, shutdown drain.
// Tests the batch LMDB flush mechanism introduced for block persistence.
// Uses real temp LMDB database files — verifies data survives process boundaries.

#include "ChunkStore.h"
#include "../Chunk/Chunk.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

static int g_tests = 0, g_passed = 0, g_failed = 0;

#define TEST(name) do { ++g_tests; test_##name(); } while(0)

#define CHECK(cond, fmt, ...) do {                                    \
    if (!(cond)) {                                                     \
        fprintf(stderr, "  FAIL [%s:%d] " fmt "\n",                   \
                 __FILE__, __LINE__, ##__VA_ARGS__);                    \
        ++g_failed;                                                    \
        return;                                                        \
    }                                                                  \
} while(0)

#define PASS() do { ++g_passed; } while(0)

static std::string makeTempDb() {
    char tmpl[] = "/tmp/chunk_dirty_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

static void removeDb(const std::string& path) {
    if (!path.empty())
        std::filesystem::remove_all(path);
}

// ---------------------------------------------------------------------------
// Test 1: basic — mark dirty, flush, persist
// ---------------------------------------------------------------------------
static void test_basic() {
    printf("TEST 1: basic\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    {
        ChunkStore store(db_path);
        store.setBlock(16, 16, 16, 42, 7);
        // setBlock calls putCached + markDirty. Now flush to LMDB.
        store.flushDirtyChunks();
    }

    // Re-open and verify
    {
        ChunkStore store(db_path);
        uint16_t id = store.GetBlockAt({16, 16, 16});
        uint8_t  meta = store.GetMeta(16, 16, 16);
        CHECK(id == 42, "expected block_id=42, got %u", id);
        CHECK(meta == 7, "expected meta=7, got %u", meta);
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 2: multiple — 10 chunks, all persist
// ---------------------------------------------------------------------------
static void test_multiple() {
    printf("TEST 2: multiple\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    constexpr int N = 10;
    {
        ChunkStore store(db_path);
        for (int i = 0; i < N; ++i) {
            store.setBlock(i * 32, 0, 0, static_cast<uint16_t>(100 + i),
                           static_cast<uint8_t>(i));
        }
        store.flushDirtyChunks();
    }

    {
        ChunkStore store(db_path);
        for (int i = 0; i < N; ++i) {
            uint16_t id = store.GetBlockAt({i * 32, 0, 0});
            uint8_t  meta = store.GetMeta(i * 32, 0, 0);
            CHECK(id == static_cast<uint16_t>(100 + i),
                  "chunk %d: expected block_id=%u, got %u", i, 100 + i, id);
            CHECK(meta == static_cast<uint8_t>(i),
                  "chunk %d: expected meta=%u, got %u", i, i, meta);
        }
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 3: same_chunk_twice — last write wins after single flush
// ---------------------------------------------------------------------------
static void test_same_chunk_twice() {
    printf("TEST 3: same_chunk_twice\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    {
        ChunkStore store(db_path);
        // Two writes to the same chunk
        store.setBlock(0, 0, 0, 100, 0);
        store.setBlock(1, 0, 0, 200, 5);
        // Both mark the same chunk dirty (set is deduplicated)
        store.flushDirtyChunks();
    }

    {
        ChunkStore store(db_path);
        // Last write should be visible (blocks[idx1]=200 only, blocks[idx0]=air because
        // setBlock copies the chunk from cache, mutates the copy, and replaces. So the
        // second setBlock overwrites the first setBlock's copy entirely.)
        // Actually: setBlock reads the chunk from cache (which has the first write),
        // modifies local idx, replaces cache. Then second setBlock reads the updated
        // chunk (which has blocks[0]=100) and writes blocks[1]=200. So both should persist.
        uint16_t id0 = store.GetBlockAt({0, 0, 0});
        uint16_t id1 = store.GetBlockAt({1, 0, 0});
        uint8_t  meta1 = store.GetMeta(1, 0, 0);
        CHECK(id0 == 100, "expected block_id=100, got %u", id0);
        CHECK(id1 == 200, "expected block_id=200, got %u", id1);
        CHECK(meta1 == 5, "expected meta=5, got %u", meta1);
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 4: evicted_skip — mark dirty, evict from cache, flush skips without crash
// ---------------------------------------------------------------------------
static void test_evicted_skip() {
    printf("TEST 4: evicted_skip\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    {
        ChunkStore store(db_path);

        // Fill cache beyond capacity (ClockCache<1024>)
        for (int i = 0; i < 1100; ++i) {
            auto* chunk = new Chunk();
            chunk->blocks[0] = static_cast<uint16_t>(i);
            store.putCached(i, 0, 0, chunk);
        }

        // Mark early chunk (likely evicted) + recent chunk (definitely in cache)
        store.markDirty(0, 0, 0);
        store.markDirty(1099, 0, 0);

        // flush — must not crash on evicted entry
        store.flushDirtyChunks();
    }

    // Recent chunk should be persisted (1099,0,0) with blocks[0]=1099
    {
        ChunkStore store(db_path);
        uint16_t id = store.GetBlockAt({1099 * 32, 0, 0});
        CHECK(id == 1099, "chunk 1099: expected block_id=1099, got %u", id);
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 5: shutdown_drain — close() flushes dirty chunks, data survives restart
// ---------------------------------------------------------------------------
static void test_shutdown_drain() {
    printf("TEST 5: shutdown_drain\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    {
        ChunkStore store(db_path);
        store.setBlock(10, 20, 30, 77, 3);
        // No explicit flush — rely on close() in destructor
    }

    {
        ChunkStore store(db_path);
        uint16_t id = store.GetBlockAt({10, 20, 30});
        uint8_t  meta = store.GetMeta(10, 20, 30);
        CHECK(id == 77, "expected block_id=77, got %u", id);
        CHECK(meta == 3, "expected meta=3, got %u", meta);
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 6: set_block_drain — setBlock path uses markDirty, drain persists it
// ---------------------------------------------------------------------------
static void test_set_block_drain() {
    printf("TEST 6: set_block_drain\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    // Multiple setBlock calls across chunks, no explicit flush
    {
        ChunkStore store(db_path);
        store.setBlock(0, 0, 0, 10, 0);
        store.setBlock(1, 1, 1, 20, 0);
        store.setBlock(31, 31, 31, 30, 0);   // last in chunk (0,0,0)
        store.setBlock(32, 32, 32, 40, 0);   // first in chunk (1,1,1)
    }

    {
        ChunkStore store(db_path);
        CHECK(store.GetBlockAt({0, 0, 0}) == 10, "got %u", store.GetBlockAt({0, 0, 0}));
        CHECK(store.GetBlockAt({1, 1, 1}) == 20, "got %u", store.GetBlockAt({1, 1, 1}));
        CHECK(store.GetBlockAt({31, 31, 31}) == 30, "got %u", store.GetBlockAt({31, 31, 31}));
        CHECK(store.GetBlockAt({32, 32, 32}) == 40, "got %u", store.GetBlockAt({32, 32, 32}));
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 7: empty_flush — flush with no dirty chunks, no crash
// ---------------------------------------------------------------------------
static void test_empty_flush() {
    printf("TEST 7: empty_flush\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    {
        ChunkStore store(db_path);
        // No modifications — flush empty set
        store.flushDirtyChunks();
        store.flushDirtyChunks();  // double flush, still empty
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 8: negative_coords — dirty tracking with negative chunk coordinates
// ---------------------------------------------------------------------------
static void test_negative_coords() {
    printf("TEST 8: negative_coords\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    {
        ChunkStore store(db_path);
        // Negative world coordinates → negative chunk coordinates
        store.setBlock(-1, -1, -1, 99, 9);
        store.setBlock(-33, -1, -1, 88, 8);  // chunk (-2, -1, -1)
        store.flushDirtyChunks();
    }

    {
        ChunkStore store(db_path);
        CHECK(store.GetBlockAt({-1, -1, -1}) == 99, "got %u", store.GetBlockAt({-1, -1, -1}));
        CHECK(store.GetMeta(-1, -1, -1) == 9, "got %u", store.GetMeta(-1, -1, -1));
        CHECK(store.GetBlockAt({-33, -1, -1}) == 88, "got %u", store.GetBlockAt({-33, -1, -1}));
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 9: concurrent_mark — 4 threads marking random chunks, flush at end
// ---------------------------------------------------------------------------
static void test_concurrent_mark() {
    printf("TEST 9: concurrent_mark\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    constexpr int kChunks = 50;
    constexpr int kThreads = 4;
    constexpr int kOps = 2000;

    {
        ChunkStore store(db_path);

        // Pre-populate cache with chunks
        for (int i = 0; i < kChunks; ++i) {
            auto* chunk = new Chunk();
            chunk->blocks[0] = static_cast<uint16_t>(1000 + i);
            store.putCached(i, 0, 0, chunk);
        }

        std::atomic<int> ready{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&store, t, &ready]() {
                unsigned seed = static_cast<unsigned>(t + 1);
                ready.fetch_add(1, std::memory_order_relaxed);
                for (int i = 0; i < kOps; ++i) {
                    int chunk_id = rand_r(&seed) % kChunks;
                    store.markDirty(chunk_id, 0, 0);
                }
            });
        }

        while (ready.load(std::memory_order_relaxed) < kThreads)
            std::this_thread::yield();

        for (auto& t : threads)
            t.join();

        store.flushDirtyChunks();
    }

    // Verify all chunks persisted
    {
        ChunkStore store(db_path);
        for (int i = 0; i < kChunks; ++i) {
            uint16_t id = store.GetBlockAt({i * 32, 0, 0});
            CHECK(id == static_cast<uint16_t>(1000 + i),
                  "chunk %d: expected %u, got %u", i, 1000 + i, id);
        }
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 10: concurrent_mark_flush — writers + flusher racing
// ---------------------------------------------------------------------------
static void test_concurrent_mark_flush() {
    printf("TEST 10: concurrent_mark_flush\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    constexpr int kChunks = 30;
    std::atomic<bool> done{false};

    {
        ChunkStore store(db_path);

        for (int i = 0; i < kChunks; ++i) {
            auto* chunk = new Chunk();
            chunk->blocks[0] = static_cast<uint16_t>(i);
            store.putCached(i, 0, 0, chunk);
        }

        std::thread writer([&]() {
            unsigned seed = 42;
            while (!done.load(std::memory_order_relaxed)) {
                int chunk_id = rand_r(&seed) % kChunks;
                store.markDirty(chunk_id, 0, 0);
                std::this_thread::yield();
            }
        });

        std::thread flusher([&]() {
            for (int i = 0; i < 30; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                store.flushDirtyChunks();
            }
            done.store(true, std::memory_order_relaxed);
        });

        writer.join();
        flusher.join();

        store.flushDirtyChunks();
    }

    {
        ChunkStore store(db_path);
        for (int i = 0; i < kChunks; ++i) {
            uint16_t id = store.GetBlockAt({i * 32, 0, 0});
            CHECK(id == static_cast<uint16_t>(i),
                  "chunk %d: expected %u, got %u", i, i, id);
        }
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 11: concurrent_stress — 8 threads marking + flushing same chunks
// ---------------------------------------------------------------------------
static void test_concurrent_stress() {
    printf("TEST 11: concurrent_stress\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    constexpr int kChunks = 20;
    std::atomic<bool> stop{false};

    {
        ChunkStore store(db_path);

        for (int i = 0; i < kChunks; ++i) {
            auto* chunk = new Chunk();
            chunk->blocks[0] = static_cast<uint16_t>(i);
            store.putCached(i, 0, 0, chunk);
        }

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&store, t, &stop]() {
                unsigned seed = static_cast<unsigned>(t + 10);
                while (!stop.load(std::memory_order_relaxed)) {
                    int chunk_id = rand_r(&seed) % kChunks;
                    store.markDirty(chunk_id, 0, 0);
                }
            });
        }

        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&store, t, &stop]() {
                unsigned seed = static_cast<unsigned>(t + 100);
                while (!stop.load(std::memory_order_relaxed)) {
                    int chunk_id = rand_r(&seed) % kChunks;
                    if (auto* c = store.getCached(chunk_id, 0, 0)) {
                        (void)c->blocks[0];
                    }
                }
            });
        }

        // Flusher thread
        std::thread flusher([&]() {
            for (int i = 0; i < 50; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                store.flushDirtyChunks();
            }
            stop.store(true, std::memory_order_relaxed);
        });

        for (auto& t : threads)
            t.join();
        flusher.join();

        store.flushDirtyChunks();
    }

    // No crash under stress is the main assertion
    // Also verify chunks are readable
    {
        ChunkStore store(db_path);
        for (int i = 0; i < kChunks; ++i) {
            uint16_t id = store.GetBlockAt({i * 32, 0, 0});
            CHECK(id == static_cast<uint16_t>(i),
                  "chunk %d: expected %u, got %u", i, i, id);
        }
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 12: mark from non-owning thread — simulate CAS handler calling markDirty
// ---------------------------------------------------------------------------
static void test_mark_from_io_thread() {
    printf("TEST 12: mark_from_io_thread\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    {
        ChunkStore store(db_path);

        // Put a chunk in cache
        auto* chunk = new Chunk();
        chunk->blocks[0] = 999;
        store.putCached(5, 5, 5, chunk);

        // Simulate CAS handler: modify chunk in-place + markDirty from another thread
        std::thread cas_thread([&]() {
            const Chunk* c = store.getCached(5, 5, 5);
            c->blocks[0] = 888;
            store.markDirty(5, 5, 5);
        });
        cas_thread.join();

        store.flushDirtyChunks();
    }

    {
        ChunkStore store(db_path);
        uint16_t id = store.GetBlockAt({5 * 32, 5 * 32, 5 * 32});
        CHECK(id == 888, "expected block_id=888, got %u", id);
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    printf("=== ChunkStore Dirty-Chunk Test ===\n\n");

    TEST(basic);
    TEST(multiple);
    TEST(same_chunk_twice);
    TEST(evicted_skip);
    TEST(shutdown_drain);
    TEST(set_block_drain);
    TEST(empty_flush);
    TEST(negative_coords);
    TEST(concurrent_mark);
    TEST(concurrent_mark_flush);
    TEST(concurrent_stress);
    TEST(mark_from_io_thread);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
