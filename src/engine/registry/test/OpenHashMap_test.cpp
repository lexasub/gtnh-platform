// OpenHashMap tests: insert/find/erase invariants, with a regression test for
// the backward-shift deletion bug where a cluster hole broke find() past it.
#include <cstdio>
#include <cstdint>

#include <engine/registry/OpenHashMap.h>

static int g_tests = 0, g_passed = 0, g_failed = 0;

static void check(bool cond, const char* file, int line, const char* expr, const char* msg) {
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
#define CHECK(cond, msg) check((cond), __FILE__, __LINE__, #cond, msg)
#define CHECK_EQ(a, b, msg) check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)

static void test_basic_insert_find_erase() {
    OpenHashMap<uint32_t, int, 16, 0> m;
    CHECK(m.empty(), "fresh map empty");
    CHECK_EQ(m.size(), 0, "fresh size 0");
    int* p = m.insert(5, 50);
    CHECK(p && *p == 50, "insert returns ptr");
    CHECK_EQ(m.size(), 1, "size after insert");
    CHECK(m.find(5) && *m.find(5) == 50, "find hit");
    CHECK(m.find(6) == nullptr, "find miss");
    CHECK(m.erase(5), "erase existing");
    CHECK_EQ(m.size(), 0, "size after erase");
    CHECK(m.find(5) == nullptr, "find after erase");
    CHECK(!m.erase(5), "erase again false");
    CHECK(m.empty(), "empty after erase");
}

static void test_insert_overwrite() {
    OpenHashMap<uint32_t, int, 16, 0> m;
    m.insert(3, 30);
    int* p = m.insert(3, 99);
    CHECK(p && *p == 99, "overwrite value");
    CHECK_EQ(m.size(), 1, "size unchanged on overwrite");
    CHECK(m.find(3) && *m.find(3) == 99, "find overwritten");
}

static void test_sentinel_rejected() {
    OpenHashMap<uint32_t, int, 8, 0> m;
    CHECK(m.insert(0, 1) == nullptr, "sentinel key rejected");
    CHECK_EQ(m.size(), 0, "size after sentinel insert attempt");
}

static void test_full_capacity() {
    OpenHashMap<uint32_t, int, 8, 0> m;
    for (uint32_t i = 1; i <= 8; ++i)
        CHECK(m.insert(i, (int)i) != nullptr, "fill slot");
    CHECK_EQ(m.size(), 8, "full size");
    CHECK(m.insert(100, 100) == nullptr, "overflow returns null");
}

static void test_clear() {
    OpenHashMap<uint32_t, int, 16, 0> m;
    m.insert(1, 1); m.insert(2, 2); m.insert(3, 3);
    m.clear();
    CHECK(m.empty(), "empty after clear");
    CHECK_EQ(m.size(), 0, "size 0 after clear");
    CHECK(m.find(1) == nullptr && m.find(2) == nullptr && m.find(3) == nullptr, "all gone");
    CHECK(m.insert(1, 42) != nullptr, "insert after clear");
}

// Regression: Capacity 8 => hash(1)==hash(9)==hash(17)==4, a 3-entry cluster at
// slots 4,5,6. Erasing the middle (9) left a hole that the old code never closed,
// so find(17) returned nullptr despite 17 being present.
static void test_cluster_erase_middle() {
    OpenHashMap<uint32_t, int, 8, 0> m;
    CHECK(m.insert(1, 100) != nullptr, "insert 1");
    CHECK(m.insert(9, 200) != nullptr, "insert 9");
    CHECK(m.insert(17, 300) != nullptr, "insert 17");
    CHECK_EQ(m.size(), 3, "size 3");
    CHECK(m.erase(9), "erase middle 9");
    CHECK_EQ(m.size(), 2, "size 2 after erase");
    CHECK(m.find(9) == nullptr, "9 gone");
    CHECK(m.find(1) && *m.find(1) == 100, "1 still findable");
    CHECK(m.find(17) && *m.find(17) == 300, "17 still findable past hole");
}

static void test_cluster_erase_head() {
    OpenHashMap<uint32_t, int, 8, 0> m;
    m.insert(1, 100); m.insert(9, 200); m.insert(17, 300);
    CHECK(m.erase(1), "erase head 1");
    CHECK_EQ(m.size(), 2, "size 2");
    CHECK(m.find(9) && *m.find(9) == 200, "9 findable");
    CHECK(m.find(17) && *m.find(17) == 300, "17 findable after head erase");
}

static void test_cluster_erase_tail() {
    OpenHashMap<uint32_t, int, 8, 0> m;
    m.insert(1, 100); m.insert(9, 200); m.insert(17, 300);
    CHECK(m.erase(17), "erase tail 17");
    CHECK_EQ(m.size(), 2, "size 2");
    CHECK(m.find(1) && *m.find(1) == 100, "1 findable");
    CHECK(m.find(9) && *m.find(9) == 200, "9 findable after tail erase");
}

static void test_stress() {
    const int CAP = 64;
    OpenHashMap<uint32_t, int, CAP, 0> m;
    bool live[CAP + 1] = {false};
    int val[CAP + 1] = {0};
    for (uint32_t i = 1; i <= (uint32_t)CAP; ++i) {
        int v = (int)(i * 7 + 3);
        CHECK(m.insert(i, v) != nullptr, "stress insert");
        live[i] = true; val[i] = v;
    }
    CHECK_EQ(m.size(), CAP, "stress full size");
    for (uint32_t i = 1; i <= (uint32_t)CAP; ++i) {
        if ((i & 1u) == 0u) {
            CHECK(m.erase(i), "stress erase");
            live[i] = false;
        }
    }
    int expected = 0;
    for (uint32_t i = 1; i <= (uint32_t)CAP; ++i) {
        if (live[i]) {
            CHECK(m.find(i) && *m.find(i) == val[i], "stress live find");
            ++expected;
        } else {
            CHECK(m.find(i) == nullptr, "stress erased miss");
        }
    }
    CHECK_EQ(m.size(), expected, "stress size matches");
    int visited = 0;
    m.for_each([&](uint32_t, int) { ++visited; });
    CHECK_EQ(visited, expected, "for_each count");
    for (uint32_t i = 1; i <= (uint32_t)CAP; ++i) {
        if (!live[i]) {
            int v = (int)(i * 7 + 3);
            CHECK(m.insert(i, v) != nullptr, "stress reinsert");
            live[i] = true; val[i] = v;
        }
    }
    CHECK_EQ(m.size(), CAP, "stress refull size");
    for (uint32_t i = 1; i <= (uint32_t)CAP; ++i)
        CHECK(m.find(i) && *m.find(i) == val[i], "stress final find");
}

int main() {
    test_basic_insert_find_erase();
    test_insert_overwrite();
    test_sentinel_rejected();
    test_full_capacity();
    test_clear();
    test_cluster_erase_middle();
    test_cluster_erase_head();
    test_cluster_erase_tail();
    test_stress();
    printf("%d/%d OpenHashMap checks passed\n", g_passed, g_tests);
    return g_failed ? 1 : 0;
}
