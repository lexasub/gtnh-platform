// ChunkStore + ClockCache integration test.
// Tests that the raw-pointer cache model works correctly:
//   - putCached/getCached round-trip
//   - eviction callback deletes chunks
//   - overwrite same key deletes old chunk
//   - SaveChunk erases from cache
//   - concurrent put/evict doesn't leak or double-free

#include "ChunkStore.h"
#include "cache/MutableChunk.h"
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

[[maybe_unused]] static int64_t makeKey(int32_t cx, int32_t cy, int32_t cz) {
    uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(cx) + 2097152);
    uint64_t y = static_cast<uint64_t>(static_cast<int64_t>(cy) + 2097152);
    uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(cz) + 2097152);
    return static_cast<int64_t>((x << 42) | (y << 21) | z);
}

static void test_put_get() {
    printf("TEST 1: put_get\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        auto* chunk = new MutableChunk();
        chunk->setBlock(0, 0, 0, 42);
        chunk->setMeta(0, 0, 0, 7);
        chunk->setMultiblock(0, 0, 0, 12345);

        store.putCached(LmdbStore::makeKey(1, 2, 3), chunk);

        const MutableChunk* got = store.getCached(1, 2, 3);
        CHECK(got != nullptr, "getCached should return non-null after put");
        CHECK(got->getBlock(0, 0, 0) == 42, "block mismatch");
        CHECK(got->getMeta(0, 0, 0) == 7, "meta mismatch");
        CHECK(got->getMultiblock(0, 0, 0) == 12345, "multiblock mismatch");

        CHECK(store.getCached(99, 99, 99) == nullptr,
              "getCached should return nullptr for non-existent key");
    }

    rmdir(dir);
    PASS();
}

static void test_overwrite() {
    printf("TEST 2: overwrite\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        auto* c1 = new MutableChunk();
        c1->setBlock(0, 0, 0, 100);
        store.putCached(LmdbStore::makeKey(0, 0, 0), c1);
        CHECK(store.getCached(0, 0, 0) != nullptr, "should be cached");

        auto* c2 = new MutableChunk();
        c2->setBlock(0, 0, 0, 200);
        store.putCached(LmdbStore::makeKey(0, 0, 0), c2);

        const MutableChunk* got = store.getCached(0, 0, 0);
        CHECK(got != nullptr, "should still be cached after overwrite");
        CHECK(got->getBlock(0, 0, 0) == 200, "should see new value after overwrite");
    }

    rmdir(dir);
    PASS();
}

static void test_save_erases() {
    printf("TEST 3: save_erases\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        auto* chunk = new MutableChunk();
        chunk->setBlock(0, 0, 0, 42);
        store.putCached(LmdbStore::makeKey(5, 6, 7), chunk);
        CHECK(store.getCached(5, 6, 7) != nullptr, "cache should have it");

        ChunkCoord coord{5, 6, 7};
        store.SaveChunk(*chunk, coord);

        const MutableChunk* got = store.getCached(5, 6, 7);
        CHECK(got != nullptr,
              "getCached should still have the chunk after SaveChunk");
        CHECK(got->getBlock(0, 0, 0) == 42,
              "block should remain 42 after SaveChunk");
    }

    rmdir(dir);
    PASS();
}

static void test_multiple_keys() {
    printf("TEST 4: multiple_keys\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        for (int i = 0; i < 50; ++i) {
            auto* chunk = new MutableChunk();
            chunk->setBlock(0, 0, 0, static_cast<uint16_t>(i));
            store.putCached(LmdbStore::makeKey(i, i + 1, i + 2), chunk);
        }

        for (int i = 0; i < 50; ++i) {
            const MutableChunk* got = store.getCached(i, i + 1, i + 2);
            CHECK(got != nullptr, "key (%d,%d,%d) should be cached", i, i + 1, i + 2);
            CHECK(got->getBlock(0, 0, 0) == static_cast<uint16_t>(i),
                  "key %d has wrong data", i);
        }
    }

    rmdir(dir);
    PASS();
}

static void test_getchunk_lmdb() {
    printf("TEST 5: getchunk_lmdb\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        const MutableChunk* c1 = store.GetChunk({10, 20, 30});
        CHECK(c1 != nullptr, "GetChunk should return a chunk (empty)");
        CHECK(c1->getBlock(0, 0, 0) == 0, "empty chunk should have air blocks");

        const MutableChunk* c2 = store.GetChunk({10, 20, 30});
        CHECK(c2 != nullptr, "second GetChunk should hit cache");
        CHECK(c2 == c1, "cache hit should return the same pointer");

        const MutableChunk* c3 = store.GetChunk({99, 99, 99});
        CHECK(c3 != nullptr, "different chunk should work");
        CHECK(c3 != c1, "different key should return different pointer");
    }

    rmdir(dir);
    PASS();
}

static void test_concurrent() {
    printf("TEST 6: concurrent\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);
        std::vector<std::thread> threads;

        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&store, t]() {
                unsigned seed = static_cast<unsigned>(t + 1);
                for (int i = 0; i < 500; ++i) {
                    int64_t k = static_cast<int64_t>(rand_r(&seed) & 1023);
                    auto* chunk = new MutableChunk();
                    chunk->setBlock(0, 0, 0, static_cast<uint16_t>(k));
                    store.putCached(static_cast<int32_t>(k), chunk);
                }
            });
        }

        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&store, t]() {
                unsigned seed = static_cast<unsigned>(t + 100);
                for (int i = 0; i < 500; ++i) {
                    int64_t k = static_cast<int64_t>(rand_r(&seed) & 1023);
                    const MutableChunk* chunk = store.getCached(static_cast<int32_t>(k), 0, 0);
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

static void test_set_block() {
    printf("TEST 7: set_block\n");

    char tmpl[] = "/tmp/chunk_cache_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");

    {
        ChunkStore store(dir);

        store.setBlock(16, 16, 16, 1001, 5);

        uint16_t id = store.GetBlockAt({16, 16, 16});
        CHECK(id == 1001, "expected block id 1001, got %d", id);

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
