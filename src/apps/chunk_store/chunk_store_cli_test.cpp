// chunk_store_cli_test.cpp
// Standalone CLI test for ChunkStore: setBlock / getBlock / getMeta / getMultiblock
// Usage: ./build/chunk_store_cli_test [db_path]
//        If db_path omitted — uses a temp directory (auto-cleaned on exit)

#include "Storage/ChunkStore.h"
#include <cstdlib>
#include <filesystem>
#include <cstdio>

// ---------------------------------------------------------------------------
// Minimal test harness, zero framework dependencies
// ---------------------------------------------------------------------------

static int  g_tests   = 0;
static int  g_passed  = 0;
static int  g_failed  = 0;

#define TEST(name)  do { ++g_tests; test_##name(); } while(0)
#define CHECK(cond, fmt, ...) do {                                    \
    if (!(cond)) {                                                     \
        fprintf(stderr, "  FAIL [%s:%d] " fmt "\n",                   \
                __FILE__, __LINE__, ##__VA_ARGS__);                    \
        ++g_failed;                                                    \
        return;                                                        \
    }                                                                  \
} while(0)
#define PASS() do { ++g_passed; } while(0)

static void report() {
    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string makeTempDb() {
    char tmpl[] = "/tmp/chunk_store_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) {
        perror("mkdtemp");
        exit(1);
    }
    return std::string(dir);
}

static void removeDb(const std::string& path) {
    std::filesystem::remove_all(path);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_basic_set_get() {
    auto db_path = makeTempDb();
    ChunkStore store(db_path);

    // set and verify one block
    store.setBlock(10, 20, 30, 42, 7);
    CHECK(store.GetBlockAt({10, 20, 30}) == 42,
          "expected block_id=42, got %u", store.GetBlockAt({10, 20, 30}));
    CHECK(store.GetMeta(10, 20, 30) == 7,
          "expected meta=7, got %u", store.GetMeta(10, 20, 30));
    CHECK(store.GetMultiblock(10, 20, 30) == 0,
          "expected mb_id=0, got %u", store.GetMultiblock(10, 20, 30));

    removeDb(db_path);
    PASS();
}

static void test_air_default() {
    auto db_path = makeTempDb();
    ChunkStore store(db_path);

    // untouched block should be air (0)
    CHECK(store.GetBlockAt({0, 0, 0}) == 0,
          "expected air (0), got %u", store.GetBlockAt({0, 0, 0}));
    CHECK(store.GetMeta(0, 0, 0) == 0,
          "expected meta=0, got %u", store.GetMeta(0, 0, 0));
    CHECK(store.GetMultiblock(0, 0, 0) == 0,
          "expected mb_id=0, got %u", store.GetMultiblock(0, 0, 0));

    removeDb(db_path);
    PASS();
}

static void test_chunk_boundary_last() {
    auto db_path = makeTempDb();
    ChunkStore store(db_path);

    // last block of chunk (0,0,0)
    store.setBlock(31, 31, 31, 100, 1);
    CHECK(store.GetBlockAt({31, 31, 31}) == 100,
          "got %u", store.GetBlockAt({31, 31, 31}));
    CHECK(store.GetMeta(31, 31, 31) == 1,
          "got %u", store.GetMeta(31, 31, 31));

    removeDb(db_path);
    PASS();
}

static void test_chunk_boundary_cross() {
    auto db_path = makeTempDb();
    ChunkStore store(db_path);

    // first block of chunk (1,0,0) — x=32 cross-chunk
    store.setBlock(32, 0, 0, 200, 3);
    CHECK(store.GetBlockAt({32, 0, 0}) == 200,
          "got %u", store.GetBlockAt({32, 0, 0}));
    CHECK(store.GetMeta(32, 0, 0) == 3,
          "got %u", store.GetMeta(32, 0, 0));

    // adjacent block in previous chunk should still be default
    CHECK(store.GetBlockAt({31, 0, 0}) == 0,
          "expected 0, got %u", store.GetBlockAt({31, 0, 0}));

    removeDb(db_path);
    PASS();
}

static void test_overwrite() {
    auto db_path = makeTempDb();
    ChunkStore store(db_path);

    store.setBlock(15, 15, 15, 1, 0);
    store.setBlock(15, 15, 15, 2, 5);
    CHECK(store.GetBlockAt({15, 15, 15}) == 2,
          "got %u", store.GetBlockAt({15, 15, 15}));
    CHECK(store.GetMeta(15, 15, 15) == 5,
          "got %u", store.GetMeta(15, 15, 15));

    removeDb(db_path);
    PASS();
}

static void test_multiple_chunks() {
    auto db_path = makeTempDb();
    ChunkStore store(db_path);

    // write blocks in 4 different chunks
    store.setBlock(  0,  0,  0, 10, 0);   // chunk ( 0, 0, 0)
    store.setBlock( 32,  0,  0, 20, 0);   // chunk ( 1, 0, 0)
    store.setBlock(  0, 32,  0, 30, 0);   // chunk ( 0, 1, 0)
    store.setBlock(  0,  0, 32, 40, 0);   // chunk ( 0, 0, 1)

    CHECK(store.GetBlockAt({ 0,  0,  0}) == 10, "got %u", store.GetBlockAt({ 0,  0,  0}));
    CHECK(store.GetBlockAt({32,  0,  0}) == 20, "got %u", store.GetBlockAt({32,  0,  0}));
    CHECK(store.GetBlockAt({ 0, 32,  0}) == 30, "got %u", store.GetBlockAt({ 0, 32,  0}));
    CHECK(store.GetBlockAt({ 0, 0, 32}) == 40, "got %u", store.GetBlockAt({ 0, 0, 32}));

    removeDb(db_path);
    PASS();
}

static void test_persistence() {
    auto db_path = makeTempDb();
    {
        ChunkStore store(db_path);
        store.setBlock(7, 8, 9, 77, 3);
    }   // store destroyed — LMDB flushed, cache gone

    {
        ChunkStore store(db_path);  // re-open same db
        CHECK(store.GetBlockAt({7, 8, 9}) == 77,
              "expected 77 after reopen, got %u", store.GetBlockAt({7, 8, 9}));
        CHECK(store.GetMeta(7, 8, 9) == 3,
              "expected meta=3 after reopen, got %u", store.GetMeta(7, 8, 9));
    }

    removeDb(db_path);
    PASS();
}

static void test_large_coords() {
    auto db_path = makeTempDb();
    ChunkStore store(db_path);

    // far-away chunk: cx=1000 => world x ~ 32000
    store.setBlock(32000, 16000, 8000, 255, 15);
    CHECK(store.GetBlockAt({32000, 16000, 8000}) == 255,
          "got %u", store.GetBlockAt({32000, 16000, 8000}));
    CHECK(store.GetMeta(32000, 16000, 8000) == 15,
          "got %u", store.GetMeta(32000, 16000, 8000));

    // negative chunk: cx=-1 => world x -1
    store.setBlock(-1, -1, -1, 99, 9);
    CHECK(store.GetBlockAt({-1, -1, -1}) == 99,
          "got %u", store.GetBlockAt({-1, -1, -1}));
    CHECK(store.GetMeta(-1, -1, -1) == 9,
          "got %u", store.GetMeta(-1, -1, -1));

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int, char**) {
    printf("=== ChunkStore CLI Test ===\n\n");

    TEST(basic_set_get);
    TEST(air_default);
    TEST(chunk_boundary_last);
    TEST(chunk_boundary_cross);
    TEST(overwrite);
    TEST(multiple_chunks);
    TEST(persistence);
    TEST(large_coords);

    report();
    return g_failed > 0 ? 1 : 0;
}
