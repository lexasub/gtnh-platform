#include "cache/ClockCache.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <thread>

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

void report() {
    printf("=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
}

// --------------------------------------------------------------------------
// Test 1: basic_get_put
// --------------------------------------------------------------------------
static void test_basic_get_put() {
    printf("TEST 1: basic_get_put\n");

    ClockCache<uintptr_t, 256> cache;

    cache.put(42, uintptr_t(100));
    CHECK(cache.get(42).has_value() && cache.get(42).value() == uintptr_t(100),
          "get(42) should return stored value");

    CHECK(cache.get(99) == std::nullopt,
          "get(99) should return nullopt (miss)");

    // Key 0 is reserved as sentinel — caller must not store under key 0.
    // get(0) on an empty cache happens to match the all-zero empty slot;
    // callers that obey the contract will never query key=0.

    PASS();
}

// --------------------------------------------------------------------------
// Test 2: overwrite same key
// --------------------------------------------------------------------------
static void test_overwrite() {
    printf("TEST 2: overwrite\n");

    ClockCache<uintptr_t, 256> cache;

    cache.put(1, uintptr_t(10));
    CHECK(cache.get(1).has_value() && cache.get(1).value() == uintptr_t(10),
          "initial value should be 10");

    cache.put(1, uintptr_t(20));
    CHECK(cache.get(1).has_value() && cache.get(1).value() == uintptr_t(20),
          "overwritten value should be 20");

    PASS();
}

// --------------------------------------------------------------------------
// Test 3: erase
// --------------------------------------------------------------------------
static void test_erase() {
    printf("TEST 3: erase\n");

    ClockCache<uintptr_t, 256> cache;

    cache.put(1, uintptr_t(10));
    CHECK(cache.get(1).has_value() && cache.get(1).value() == uintptr_t(10),
          "value should be present before erase");

    cache.erase(1);
    CHECK(cache.get(1) == std::nullopt,
          "value should be removed after erase");

    cache.erase(999);  // erase non-existing key should not crash
    PASS();
}

// --------------------------------------------------------------------------
// Test 4: negative key
// --------------------------------------------------------------------------
static void test_negative_key() {
    printf("TEST 4: negative_key\n");

    ClockCache<uintptr_t, 256> cache;

    cache.put(-5, uintptr_t(42));
    CHECK(cache.get(-5).has_value() && cache.get(-5).value() == uintptr_t(42),
          "negative key -5 should work");

    cache.put(INT64_MIN + 1, uintptr_t(42));
    CHECK(cache.get(INT64_MIN + 1).has_value() &&
          cache.get(INT64_MIN + 1).value() == uintptr_t(42),
          "INT64_MIN + 1 should work");

    PASS();
}

// --------------------------------------------------------------------------
// Test 5: multiple keys — fit within capacity
// --------------------------------------------------------------------------
static void test_multiple_keys() {
    printf("TEST 5: multiple_keys\n");

    // Capacity=256 → 64 sets × 4 slots. 120 keys fits easily (~2 per set).
    ClockCache<uintptr_t, 256> cache;

    for (int i = 1; i <= 120; ++i) {
        cache.put(i, uintptr_t(1000 + i));
    }

    for (int i = 1; i <= 120; ++i) {
        auto v = cache.get(i);
        CHECK(v.has_value() && v.value() == uintptr_t(1000 + i),
              "key %d should contain value %lu", i, (unsigned long)(1000 + i));
    }

    // Key not inserted
    CHECK(cache.get(9999) == std::nullopt,
          "key 9999 should be a miss");

    PASS();
}

// --------------------------------------------------------------------------
// Test 6: eviction under pressure
// --------------------------------------------------------------------------
static void test_eviction() {
    printf("TEST 6: eviction\n");

    // Capacity=4 → 1 set × 4 slots. Only 4 entries fit at any time.
    ClockCache<uintptr_t, 4> cache;

    for (int i = 1; i <= 8; ++i) {
        cache.put(i, uintptr_t(100 + i));
    }

    // At most 4 of the 8 keys survive. Check the invariant.
    size_t alive = 0;
    for (int i = 1; i <= 8; ++i) {
        if (cache.get(i).has_value())
            ++alive;
    }
    CHECK(alive > 0 && alive <= 4,
          "expected 1-4 survivors after 8 inserts into 4-slot cache, got %zu", alive);

    PASS();
}

// --------------------------------------------------------------------------
// Test 7: concurrent multi-threaded stress
// --------------------------------------------------------------------------
static void test_concurrent() {
    printf("TEST 7: concurrent (multi-threaded stress)\n");

    ClockCache<uintptr_t, 4096> cache;  // 1024 sets × 4 ways
    constexpr int kOps = 5000;

    std::vector<std::thread> threads;

    threads.emplace_back([&]() {
        unsigned seed = 1;
        for (int i = 0; i < kOps; ++i) {
            int64_t key = static_cast<int64_t>(rand_r(&seed) & 8191);
            cache.put(key, uintptr_t(1000 + (key & 0xFF)));
        }
    });

    threads.emplace_back([&]() {
        unsigned seed = 2;
        for (int i = 0; i < kOps; ++i) {
            int64_t key = static_cast<int64_t>(rand_r(&seed) & 8191);
            auto val = cache.get(key);
            (void)val;
        }
    });

    threads.emplace_back([&]() {
        unsigned seed = 3;
        for (int i = 0; i < kOps; ++i) {
            int64_t key = static_cast<int64_t>(rand_r(&seed) & 8191);
            cache.erase(key);
        }
    });

    threads.emplace_back([&]() {
        unsigned seed = 4;
        for (int i = 0; i < kOps; ++i) {
            int64_t key = static_cast<int64_t>(rand_r(&seed) & 8191);
            auto val = cache.get(key);
            (void)val;
        }
    });

    for (auto& t : threads) {
        t.join();
    }

    // Cache should still be functional after stress
    cache.put(12345, uintptr_t(0xDEADBEEF));
    CHECK(cache.get(12345).has_value() &&
          cache.get(12345).value() == uintptr_t(0xDEADBEEF),
          "cache should still work after stress test");

    PASS();
}

// --------------------------------------------------------------------------
// Test 8: all negative keys
// --------------------------------------------------------------------------
static void test_all_negative_keys() {
    printf("TEST 8: all_negative_keys\n");

    ClockCache<uintptr_t, 256> cache;

    cache.put(-1, uintptr_t(1));
    CHECK(cache.get(-1).has_value() && cache.get(-1).value() == uintptr_t(1),
          "key -1 should work");

    cache.put(-2, uintptr_t(2));
    CHECK(cache.get(-2).has_value() && cache.get(-2).value() == uintptr_t(2),
          "key -2 should work");

    cache.put(-3, uintptr_t(3));
    CHECK(cache.get(-3).has_value() && cache.get(-3).value() == uintptr_t(3),
          "key -3 should work");

    cache.put(-1000, uintptr_t(1000));
    CHECK(cache.get(-1000).has_value() && cache.get(-1000).value() == uintptr_t(1000),
          "key -1000 should work");

    PASS();
}

// --------------------------------------------------------------------------
// Test 9: eviction callback — on_evict fires on eviction (pass 3)
// --------------------------------------------------------------------------
static void test_evict_cb_eviction() {
    printf("TEST 9: evict_cb_eviction\n");

    ClockCache<uintptr_t, 4> cache;  // 1 set, 4 slots
    int evict_count = 0;
    uintptr_t evicted_value = 0;
    cache.set_on_evict([&](uintptr_t val) { ++evict_count; evicted_value = val; });

    // Fill all 4 slots
    for (int i = 1; i <= 4; ++i)
        cache.put(i, uintptr_t(100 + i));
    CHECK(evict_count == 0, "no evictions before capacity exceeded");

    // Insert 5th — forces eviction of one existing entry
    cache.put(5, uintptr_t(200));
    CHECK(evict_count == 1, "expected 1 eviction, got %d", evict_count);
    CHECK(evicted_value >= uintptr_t(101) && evicted_value <= uintptr_t(104),
          "evicted value %lu should be one of the original entries",
          (unsigned long)evicted_value);

    // At most 4 entries survive
    size_t alive = 0;
    for (int i = 1; i <= 5; ++i)
        if (cache.get(i).has_value()) ++alive;
    CHECK(alive > 0 && alive <= 4,
          "expected 1-4 survivors, got %zu", alive);

    PASS();
}

// --------------------------------------------------------------------------
// Test 10: eviction callback — on_evict fires on same-key overwrite (pass 1)
// --------------------------------------------------------------------------
static void test_evict_cb_overwrite() {
    printf("TEST 10: evict_cb_overwrite\n");

    ClockCache<uintptr_t, 256> cache;
    int evict_count = 0;
    uintptr_t evicted_value = 0;
    cache.set_on_evict([&](uintptr_t val) { ++evict_count; evicted_value = val; });

    cache.put(42, uintptr_t(100));
    CHECK(evict_count == 0, "no eviction on first insert");

    cache.put(42, uintptr_t(200));  // same key — triggers pass 1
    CHECK(evict_count == 1, "expected 1 eviction on overwrite, got %d", evict_count);
    CHECK(evicted_value == uintptr_t(100),
          "evicted value should be the old value (100), got %lu",
          (unsigned long)evicted_value);
    CHECK(cache.get(42).has_value() && cache.get(42).value() == uintptr_t(200),
          "new value should be 200 after overwrite");

    PASS();
}

// --------------------------------------------------------------------------
// Test 11: eviction callback — on_evict fires on erase
// --------------------------------------------------------------------------
static void test_evict_cb_erase() {
    printf("TEST 11: evict_cb_erase\n");

    ClockCache<uintptr_t, 256> cache;
    int evict_count = 0;
    cache.set_on_evict([&](uintptr_t) { ++evict_count; });

    cache.put(1, uintptr_t(10));
    cache.put(2, uintptr_t(20));
    CHECK(evict_count == 0, "no evictions before erase");

    cache.erase(1);
    CHECK(evict_count == 1, "expected 1 eviction on erase, got %d", evict_count);

    cache.erase(999);  // non-existing key — no callback
    CHECK(evict_count == 1, "no extra eviction for non-existing key");

    PASS();
}

// --------------------------------------------------------------------------
// Test 12: eviction callback — on_evict fires on clear
// --------------------------------------------------------------------------
static void test_evict_cb_clear() {
    printf("TEST 12: evict_cb_clear\n");

    ClockCache<uintptr_t, 256> cache;
    int evict_count = 0;
    uintptr_t evicted[4];
    cache.set_on_evict([&](uintptr_t val) { evicted[evict_count++] = val; });

    cache.put(1, uintptr_t(10));
    cache.put(2, uintptr_t(20));
    cache.put(3, uintptr_t(30));
    cache.put(4, uintptr_t(40));

    cache.clear();
    CHECK(evict_count == 4, "expected 4 evictions on clear, got %d", evict_count);

    // All entries gone after clear
    CHECK(cache.get(1) == std::nullopt, "key 1 should be gone after clear");
    CHECK(cache.get(2) == std::nullopt, "key 2 should be gone after clear");
    CHECK(cache.get(3) == std::nullopt, "key 3 should be gone after clear");
    CHECK(cache.get(4) == std::nullopt, "key 4 should be gone after clear");

    PASS();
}

// --------------------------------------------------------------------------
// Test 13: clear re-usable after clear
// --------------------------------------------------------------------------
static void test_clear_reuse() {
    printf("TEST 13: clear_reuse\n");

    ClockCache<uintptr_t, 256> cache;
    int evict_count = 0;
    cache.set_on_evict([&](uintptr_t) { ++evict_count; });

    cache.put(1, uintptr_t(10));
    cache.put(2, uintptr_t(20));
    cache.clear();
    CHECK(evict_count == 2, "expected 2 evictions on first clear");

    cache.put(1, uintptr_t(100));
    cache.put(2, uintptr_t(200));
    CHECK(cache.get(1).has_value() && cache.get(1).value() == uintptr_t(100),
          "re-inserted key 1 should work after clear");
    CHECK(cache.get(2).has_value() && cache.get(2).value() == uintptr_t(200),
          "re-inserted key 2 should work after clear");

    PASS();
}

int main() {
    TEST(basic_get_put);
    TEST(overwrite);
    TEST(erase);
    TEST(negative_key);
    TEST(multiple_keys);
    TEST(eviction);
    TEST(concurrent);
    TEST(all_negative_keys);
    TEST(evict_cb_eviction);
    TEST(evict_cb_overwrite);
    TEST(evict_cb_erase);
    TEST(evict_cb_clear);
    TEST(clear_reuse);

    report();
    return g_failed > 0 ? 1 : 0;
}
