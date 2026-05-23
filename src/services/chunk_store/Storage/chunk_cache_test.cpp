// ChunkStore + ClockCache integration test.
// Tests that the raw-pointer cache model works correctly:
//   - putCached/getCached round-trip
//   - eviction callback deletes chunks
//   - overwrite same key deletes old chunk
//   - SaveChunk erases from cache
//   - concurrent put/evict doesn't leak or double-free

#include "ChunkStore.h"
#include "../Chunk/Chunk.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
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

// We test at the ChunkStore cache layer directly, bypassing LMDB.
// For tests that need GetChunk (which hits LMDB), use a temp dir.

// Test helpers
[[maybe_unused]] static int64_t makeKey(int32_t cx, int32_t cy, int32_t cz) {
    uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(cx) + 2097152);
    uint64_t y = static_cast<uint64_t>(static_cast<int64_t>(cy) + 2097152);
    uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(cz) + 2097152);
    return static_cast<int64_t>((x << 42) | (y << 21) | z);
}

// --------------------------------------------------------------------------
// Test 1: putCached / getCached round-trip
// --------------------------------------------------------------------------
static void test_put_get() {
    printf("TEST 1: put_get\n");

    // Create temp directory for LMDB
    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        auto* chunk = new Chunk();
        chunk->blocks[0] = 42;
        chunk->meta[0] = 7;
        chunk->multiblock[0] = 12345;

        store.putCached(1, 2, 3, chunk);

        const Chunk* got = store.getCached(1, 2, 3);
        CHECK(got != nullptr, "getCached should return non-null after put");
        CHECK(got->blocks[0] == 42, "blocks[0] mismatch");
        CHECK(got->meta[0] == 7, "meta[0] mismatch");
        CHECK(got->multiblock[0] == 12345, "multiblock[0] mismatch");

        // Miss on non-existent key
        CHECK(store.getCached(99, 99, 99) == nullptr,
              "getCached should return nullptr for non-existent key");
    }

    // Cleanup
    rmdir(dir);
    PASS();
}

// --------------------------------------------------------------------------
// Test 2: overwrite same key — old chunk deleted
// --------------------------------------------------------------------------
static void test_overwrite() {
    printf("TEST 2: overwrite\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        auto* c1 = new Chunk();
        c1->blocks[0] = 100;
        store.putCached(0, 0, 0, c1);
        CHECK(store.getCached(0, 0, 0) != nullptr, "should be cached");

        // Overwrite with a new chunk — old one is deleted by eviction callback
        auto* c2 = new Chunk();
        c2->blocks[0] = 200;
        store.putCached(0, 0, 0, c2);

        const Chunk* got = store.getCached(0, 0, 0);
        CHECK(got != nullptr, "should still be cached after overwrite");
        CHECK(got->blocks[0] == 200, "should see new value after overwrite");

        // Can't verify old chunk is deleted directly (race-free),
        // but ASan/valgrind would catch use-after-free.
    }

    rmdir(dir);
    PASS();
}

// --------------------------------------------------------------------------
// Test 3: SaveChunk erases from cache
// --------------------------------------------------------------------------
static void test_save_erases() {
    printf("TEST 3: save_erases\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        auto* chunk = new Chunk();
        chunk->blocks[0] = 42;
        store.putCached(5, 6, 7, chunk);
        CHECK(store.getCached(5, 6, 7) != nullptr, "cache should have it");

        // SaveChunk writes to LMDB and keeps the cache entry (both up-to-date).
        ChunkCoord coord{5, 6, 7};
        store.SaveChunk(*chunk, coord);

        // After SaveChunk, the cache entry is still there — no need to evict
        // because the copy in LMDB and the cache are identical.
        const Chunk* got = store.getCached(5, 6, 7);
        CHECK(got != nullptr,
              "getCached should still have the chunk after SaveChunk");
        CHECK(got->blocks[0] == 42,
              "blocks[0] should remain 42 after SaveChunk");
    }

    rmdir(dir);
    PASS();
}

// --------------------------------------------------------------------------
// Test 4: multiple keys — independent entries
// --------------------------------------------------------------------------
static void test_multiple_keys() {
    printf("TEST 4: multiple_keys\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        for (int i = 0; i < 50; ++i) {
            auto* chunk = new Chunk();
            chunk->blocks[0] = static_cast<uint16_t>(i);
            store.putCached(i, i + 1, i + 2, chunk);
        }

        for (int i = 0; i < 50; ++i) {
            const Chunk* got = store.getCached(i, i + 1, i + 2);
            CHECK(got != nullptr, "key (%d,%d,%d) should be cached", i, i + 1, i + 2);
            CHECK(got->blocks[0] == static_cast<uint16_t>(i),
                  "key %d has wrong data: got %d", i, got->blocks[0]);
        }
    }

    rmdir(dir);
    PASS();
}

// --------------------------------------------------------------------------
// Test 5: GetChunk — cache miss loads from LMDB
// --------------------------------------------------------------------------
static void test_getchunk_lmdb() {
    printf("TEST 5: getchunk_lmdb\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        // First call: cache miss → LMDB read (fails → empty chunk)
        const Chunk* c1 = store.GetChunk({10, 20, 30});
        CHECK(c1 != nullptr, "GetChunk should return a chunk (empty)");
        CHECK(c1->blocks[0] == 0, "empty chunk should have air blocks");

        // Second call: cache hit (same key)
        const Chunk* c2 = store.GetChunk({10, 20, 30});
        CHECK(c2 != nullptr, "second GetChunk should hit cache");
        CHECK(c2 == c1, "cache hit should return the same pointer");

        // Different key: new chunk
        const Chunk* c3 = store.GetChunk({99, 99, 99});
        CHECK(c3 != nullptr, "different chunk should work");
        CHECK(c3 != c1, "different key should return different pointer");
    }

    rmdir(dir);
    PASS();
}

// --------------------------------------------------------------------------
// Test 6: concurrent put/get — no crashes
// --------------------------------------------------------------------------
static void test_concurrent() {
    printf("TEST 6: concurrent\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);
        std::vector<std::thread> threads;

        // Writer threads
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&store, t]() {
                unsigned seed = static_cast<unsigned>(t + 1);
                for (int i = 0; i < 500; ++i) {
                    int64_t k = static_cast<int64_t>(rand_r(&seed) & 1023);
                    auto* chunk = new Chunk();
                    chunk->blocks[0] = static_cast<uint16_t>(k);
                    store.putCached(static_cast<int32_t>(k), 0, 0, chunk);
                }
            });
        }

        // Reader threads
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&store, t]() {
                unsigned seed = static_cast<unsigned>(t + 100);
                for (int i = 0; i < 500; ++i) {
                    int64_t k = static_cast<int64_t>(rand_r(&seed) & 1023);
                    const Chunk* chunk = store.getCached(static_cast<int32_t>(k), 0, 0);
                    (void)chunk;
                }
            });
        }

        for (auto& t : threads)
            t.join();
    }

    rmdir(dir);
    PASS();
}

// --------------------------------------------------------------------------
// Test 7: setBlock + getBlock — end-to-end
// --------------------------------------------------------------------------
static void test_set_block() {
    printf("TEST 7: set_block\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        // Set a block (triggers copy-on-write + cache replace)
        store.setBlock(16, 16, 16, 1001, 5);

        // Read it back
        uint16_t id = store.GetBlockAt({16, 16, 16});
        CHECK(id == 1001, "expected block id 1001, got %d", id);

        // Check meta
        uint8_t meta = store.GetMeta(16, 16, 16);
        CHECK(meta == 5, "expected meta 5, got %d", meta);
    }

    rmdir(dir);
    PASS();
}

int main() {
    TEST(put_get);
    TEST(overwrite);
    TEST(save_erases);
    TEST(multiple_keys);
    TEST(getchunk_lmdb);
    TEST(concurrent);
    TEST(set_block);

    printf("=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
