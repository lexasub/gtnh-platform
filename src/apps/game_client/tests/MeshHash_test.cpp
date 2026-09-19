// Mesh hash tests: FNV-1a over block ids + meta. The key invariant is that a
// meta-only change must alter the hash — the bug being fixed ignored meta, so
// pipe-connection toggles did not trigger a client mesh rebuild.
#include <cstdio>
#include <cstdint>

#include "Cache/MeshHash.h"

static int g_tests = 0, g_passed = 0, g_failed = 0;

static void test_check(bool cond, const char* file, int line, const char* expr, const char* msg) {
    ++g_tests;
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}
#define CHECK(cond, msg) test_check((cond), __FILE__, __LINE__, #cond, msg)
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)

static void test_deterministic() {
    const uint16_t blocks[3] = {1, 2, 3};
    const uint8_t meta[3] = {0, 0, 0};
    CHECK_EQ(mesh::HashBlockData(blocks, 3, meta, 3),
             mesh::HashBlockData(blocks, 3, meta, 3),
             "same input -> same hash");
}

static void test_meta_only_change() {
    const uint16_t blocks[3] = {1, 2, 3};
    const uint8_t m0[3] = {0, 0, 0};
    const uint8_t m1[3] = {1, 0, 0};
    CHECK(mesh::HashBlockData(blocks, 3, m0, 3) != mesh::HashBlockData(blocks, 3, m1, 3),
          "meta-only change -> different hash");
}

static void test_block_id_change() {
    const uint16_t b0[3] = {1, 2, 3};
    const uint16_t b1[3] = {1, 2, 4};
    const uint8_t meta[3] = {0, 0, 0};
    CHECK(mesh::HashBlockData(b0, 3, meta, 3) != mesh::HashBlockData(b1, 3, meta, 3),
          "block-id change -> different hash");
}

static void test_empty_input() {
    CHECK_EQ(mesh::HashBlockData(nullptr, 0, nullptr, 0),
             uint64_t(0xcbf29ce484222325ull),
             "empty input -> FNV offset basis");
}

int main() {
    test_deterministic();
    test_meta_only_change();
    test_block_id_change();
    test_empty_input();
    printf("%d/%d mesh hash checks passed\n", g_passed, g_tests);
    return g_failed ? 1 : 0;
}
