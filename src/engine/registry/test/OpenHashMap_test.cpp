// OpenHashMap tests: insert/find/erase invariants, with a regression test for
// the backward-shift deletion bug where a cluster hole broke find() past it.
#include <cstdio>
#include <cstdint>
#include <vector>

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

// ---------------------------------------------------------------------------
// gp-2kb: the paths the pre-existing tests above do not reach.
//
// Note on the ticket's wording: OpenHashMap is a FIXED-CAPACITY, zero-allocation
// open-addressed table. There is no rehash and no load-factor-triggered growth
// anywhere in OpenHashMap.h — Capacity is a template parameter and entries_ is
// a plain `Entry entries_[Capacity]` inline array, so "growth/rehash at a
// load-factor boundary" and "many keys forcing several rehashes" have no
// production code to exercise. What DOES exist at the boundary is the
// fall-through: insert() returning nullptr once every slot is occupied, and —
// the part that is genuinely untested and genuinely subtle — the
// backward-shift erase running on a COMPLETELY FULL table, where no empty
// slot exists to terminate the closing scan. The tests below cover the real
// boundary behaviour instead of a growth path that does not exist.
// ---------------------------------------------------------------------------

// For Capacity 8 the hash is (key * 2654435761) >> 29 & 7. Measured with the
// exact constants in OpenHashMap.h:
//   key  1 -> slot 4    key 5 -> slot 0
//   key  3 -> slot 6    key 6 -> slot 5
//   key 11 -> slot 7    key 24 -> slot 0
// These are asserted, not assumed: if the hash ever changes, the cluster
// layouts these tests depend on shift and the failures point at that.
static void test_hash_layout_is_what_clusters_assume() {
    // The three collisions the existing cluster tests rely on: 1, 9, 17 all
    // land on slot 4 at Capacity 8. Only the values are meaningful here; the
    // layout itself is verified behaviourally in the cluster tests below.
    OpenHashMap<uint32_t, int, 8, 0> probe;
    // Insert 1, 9, 17 — if they did NOT collide, they would occupy three
    // separate slots and the "cluster" comments above would be false.
    probe.insert(1, 1);
    probe.insert(9, 9);
    probe.insert(17, 17);
    // Erase the head and the middle. With three entries sharing one slot the
    // backward shift MUST move the tail; a broken shift shows up as a find miss.
    CHECK(probe.erase(1), "erase head of the 1/9/17 cluster");
    CHECK(probe.find(17) != nullptr, "tail 17 survives a head erase");
    CHECK_EQ(2, probe.size(), "size after head erase");

    // A table where the three keys provably land in three distinct slots must
    // NOT exhibit the shift. Keys 1, 2, 3 hash to slots 4, 1, 6.
    OpenHashMap<uint32_t, int, 8, 0> spread;
    spread.insert(1, 1);
    spread.insert(2, 2);
    spread.insert(3, 3);
    CHECK(spread.erase(1), "erase key 1 from a collision-free table");
    CHECK(spread.find(2) != nullptr, "2 unaffected by erasing 1");
    CHECK(spread.find(3) != nullptr, "3 unaffected by erasing 1");
    CHECK_EQ(2, spread.size(), "size after erase in spread table");
}

// The load-factor boundary: at exactly Capacity entries the table is full, and
// the next insert must be refused rather than overwriting a live entry.
static void test_load_factor_boundary() {
    OpenHashMap<uint32_t, int, 8, 0> m;
    // Fill to capacity - 1: still one free slot.
    for (uint32_t i = 1; i <= 7; ++i)
        CHECK(m.insert(i, (int)i) != nullptr, "fill to capacity-1");
    CHECK_EQ(m.size(), 7, "size at capacity-1");

    // The last free slot is accepted.
    CHECK(m.insert(8, 8) != nullptr, "the capacity-th insert is accepted");
    CHECK_EQ(m.size(), 8, "size at exactly capacity");

    // One past capacity is refused, and — the part worth asserting — the
    // refusal must not have disturbed any live entry.
    CHECK(m.insert(9, 9) == nullptr, "insert past capacity refused");
    CHECK_EQ(m.size(), 8, "size unchanged after refused insert");
    for (uint32_t i = 1; i <= 8; ++i)
        CHECK(m.find(i) && *m.find(i) == (int)i, "live entry survives a refused insert");

    // Overwriting an existing key at full capacity is still legal and must not
    // grow size.
    CHECK(m.insert(4, 444) != nullptr, "overwrite at full capacity");
    CHECK_EQ(m.size(), 8, "overwrite at capacity does not grow size");
    CHECK(m.find(4) && *m.find(4) == 444, "overwrite at capacity took effect");
    CHECK(m.find(5) && *m.find(5) == 5, "overwrite at capacity did not clobber a neighbour");
}

// Backward-shift deletion on a COMPLETELY FULL table. There is no empty slot,
// so the closing scan cannot terminate on "entries_[next] == SentinelKey"; it
// runs the full Capacity-1 bound and relies on wrapping back to the hole.
// The old hole-and-skip bug was invisible whenever a table had a free slot.
static void test_erase_all_from_full_table() {
    OpenHashMap<uint32_t, int, 8, 0> m;
    for (uint32_t i = 1; i <= 8; ++i)
        CHECK(m.insert(i, (int)i) != nullptr, "fill table");
    CHECK_EQ(m.size(), 8, "full");

    // Erase every key in ascending order. After each erase the table is one
    // hole deeper, and every surviving key must still be reachable.
    for (uint32_t gone = 1; gone <= 8; ++gone) {
        CHECK(m.erase(gone), "erase from full table");
        CHECK_EQ(m.size(), (int)(8 - gone), "size after erase");
        CHECK(m.find(gone) == nullptr, "erased key does not reappear");
        for (uint32_t live = gone + 1; live <= 8; ++live)
            CHECK(m.find(live) && *m.find(live) == (int)live,
                  "survivor still findable after erase from a full table");
    }
    CHECK(m.empty(), "empty after erasing a full table completely");

    // Same thing in descending order — the scan direction relative to the
    // cluster is different, so it is a genuinely separate walk.
    OpenHashMap<uint32_t, int, 8, 0> d;
    for (uint32_t i = 1; i <= 8; ++i)
        d.insert(i, (int)i);
    for (uint32_t gone = 8; gone >= 1; --gone) {
        CHECK(d.erase(gone), "erase descending from full table");
        CHECK_EQ(d.size(), (int)(gone - 1), "size after descending erase");
        for (uint32_t live = 1; live < gone; ++live)
            CHECK(d.find(live) && *d.find(live) == (int)live,
                  "survivor still findable after descending erase");
    }
    CHECK(d.empty(), "empty after descending drain");
}

// A probe chain that wraps the array end. hash(3)=6, hash(11)=7, hash(24)=0
// at Capacity 8, so the chain 3 -> 11 -> 24 occupies slots 6, 7, 0 and the
// last insert crossed the array boundary. Backward-shift must close a hole at
// slot 6 by sliding entries across that boundary, which the `ideal <= next`
// branch of the on-path test handles differently from a non-wrapping chain.
static void test_wraparound_probe_chain_erase() {
    // head of the wrapping chain
    {
        OpenHashMap<uint32_t, int, 8, 0> m;
        CHECK(m.insert(3, 300) != nullptr, "insert 3 (slot 6)");
        CHECK(m.insert(11, 1100) != nullptr, "insert 11 (slot 7)");
        CHECK(m.insert(24, 2400) != nullptr, "insert 24 (slot 0, wraps)");
        CHECK_EQ(m.size(), 3, "wrapping chain size");
        CHECK(m.erase(3), "erase head of wrapping chain");
        CHECK(m.find(3) == nullptr, "head gone");
        CHECK(m.find(11) && *m.find(11) == 1100, "11 shifted back across the boundary");
        CHECK(m.find(24) && *m.find(24) == 2400, "24 shifted back across the boundary");
        CHECK_EQ(m.size(), 2, "size after wrapping head erase");
    }
    // middle of the wrapping chain
    {
        OpenHashMap<uint32_t, int, 8, 0> m;
        m.insert(3, 300); m.insert(11, 1100); m.insert(24, 2400);
        CHECK(m.erase(11), "erase middle of wrapping chain");
        CHECK(m.find(11) == nullptr, "middle gone");
        CHECK(m.find(3) && *m.find(3) == 300, "3 still findable");
        CHECK(m.find(24) && *m.find(24) == 2400, "24 findable past the wrapping hole");
    }
    // tail of the wrapping chain
    {
        OpenHashMap<uint32_t, int, 8, 0> m;
        m.insert(3, 300); m.insert(11, 1100); m.insert(24, 2400);
        CHECK(m.erase(24), "erase tail of wrapping chain");
        CHECK(m.find(24) == nullptr, "tail gone");
        CHECK(m.find(3) && *m.find(3) == 300, "3 findable");
        CHECK(m.find(11) && *m.find(11) == 1100, "11 findable");
    }
}

// Removal followed by lookup of the removed key, checked at every erase
// position in a dense table — this is the "removal then lookup" case the
// ticket asks for, at a size the 3-key cluster tests do not reach.
static void test_erase_then_lookup_is_consistent() {
    constexpr int CAP = 32;
    OpenHashMap<uint32_t, int, CAP, 0> m;
    for (uint32_t i = 1; i <= CAP; ++i)
        CHECK(m.insert(i, (int)(i * 3)) != nullptr, "seed");

    // Erase every third key; the survivors include keys that were displaced
    // into later slots by linear probing.
    for (uint32_t i = 1; i <= CAP; i += 3) {
        CHECK(m.erase(i), "erase every third");
        CHECK(m.find(i) == nullptr, "erased key misses");
        // Every still-live key stays findable, checked immediately after the
        // erase that could have broken its probe chain.
        for (uint32_t live = 1; live <= CAP; ++live) {
            if (live <= i && (live % 3) == 1)
                continue;  // already erased
            CHECK(m.find(live) != nullptr, "live key findable right after an erase");
        }
    }
    int live_count = 0;
    for (uint32_t i = 1; i <= CAP; ++i)
        if ((i % 3) != 1)
            ++live_count;
    CHECK_EQ(m.size(), live_count, "size matches the number of survivors");

    // Re-inserting an erased key works and does not disturb the survivors.
    for (uint32_t i = 1; i <= CAP; i += 3) {
        CHECK(m.insert(i, (int)(i * 3)) != nullptr, "reinsert erased key");
        CHECK(m.find(i) && *m.find(i) == (int)(i * 3), "reinserted value correct");
    }
    CHECK_EQ(m.size(), CAP, "back to full");
    for (uint32_t i = 1; i <= CAP; ++i)
        CHECK(m.find(i) && *m.find(i) == (int)(i * 3), "final state correct");
}

// The iteration-order contract. for_each walks the slot array from index 0, so
// the visit order is a function of the hash layout, NOT of insertion order.
// A caller that assumes insertion order is wrong; this test pins the actual
// order so that a future change to it is a deliberate, visible break.
static void test_iteration_order_is_slot_order_not_insertion_order() {
    OpenHashMap<uint32_t, int, 8, 0> m;
    // Insert in an order that differs from slot order.
    const uint32_t ins[8] = {8, 1, 2, 3, 4, 5, 6, 7};
    for (uint32_t k : ins)
        CHECK(m.insert(k, (int)k) != nullptr, "seed in scrambled order");

    std::vector<uint32_t> visited;
    m.for_each([&](uint32_t k, int) { visited.push_back(k); });

    // Slot layout for keys 1..8 at Capacity 8 is
    //   slot: 0 1 2 3 4 5 6 7
    //   key : 5 2 7 4 1 6 3 8
    const uint32_t expect[8] = {5, 2, 7, 4, 1, 6, 3, 8};
    CHECK_EQ(visited.size(), (size_t)8, "for_each visited every entry");
    for (int i = 0; i < 8; ++i)
        CHECK(visited[i] == expect[i], "for_each yields slot order, not insertion order");

    // Order is repeatable: a second walk of an unmodified table is identical.
    std::vector<uint32_t> again;
    m.for_each([&](uint32_t k, int) { again.push_back(k); });
    CHECK(again == visited, "for_each order is stable across calls");

    // for_each skips holes but still walks the whole array, so erasing an
    // entry removes it from the sequence without reshuffling the rest.
    CHECK(m.erase(4), "erase slot 3");
    std::vector<uint32_t> after;
    m.for_each([&](uint32_t k, int) { after.push_back(k); });
    CHECK_EQ(after.size(), (size_t)7, "one fewer after erase");
    for (int i = 0, j = 0; i < 8; ++i) {
        if (expect[i] == 4)
            continue;
        CHECK(after[j] == expect[i], "remaining order preserved after erase");
        ++j;
    }

    // A find() walk never terminates on a hole past a live key: with 4 erased
    // the table still answers for everything it holds.
    for (uint32_t k = 1; k <= 8; ++k) {
        if (k == 4) {
            CHECK(m.find(k) == nullptr, "4 misses after erase");
        } else {
            CHECK(m.find(k) && *m.find(k) == (int)k, "live key findable with a hole present");
        }
    }
}

// Many keys, no growth — the closest honest analogue to "many keys forcing
// several rehashes": hold the table at a sustained high load factor while
// inserting, looking up and erasing, which is where a fixed-capacity table
// would break if its rehash-free invariants were wrong.
static void test_sustained_high_load() {
    constexpr int CAP = 256;
    // Keys 1..64 are the permanent set: they are inserted once and never
    // touched again, so their survival across the churn is a real assertion.
    // Keys 65..256 are the churn set, drained and refilled in three waves.
    constexpr int PERMANENT = 64;
    constexpr int WAVE = (CAP - PERMANENT) / 3;  // 64 keys per wave
    OpenHashMap<uint32_t, int, CAP, 0> m;
    // Phase 1: fill to exactly capacity.
    for (uint32_t i = 1; i <= CAP; ++i)
        CHECK(m.insert(i, (int)i) != nullptr, "fill to capacity");
    CHECK_EQ(m.size(), CAP, "at capacity");
    // Phase 2: a fully loaded table must refuse, never corrupt.
    for (uint32_t i = 1; i <= 32; ++i)
        CHECK(m.insert(CAP + i, (int)i) == nullptr, "refused while full");
    CHECK_EQ(m.size(), CAP, "size held through refusals");

    // Phase 3: churn. Free one wave, refill it with fresh keys, three times.
    // The table returns to full capacity after every wave.
    for (int wave = 0; wave < 3; ++wave) {
        uint32_t lo = PERMANENT + 1 + wave * WAVE;
        for (uint32_t k = lo; k < lo + WAVE; ++k)
            CHECK(m.erase(k), "churn erase");
        CHECK_EQ(m.size(), CAP - WAVE, "size after churn erase");
        for (uint32_t k = lo; k < lo + WAVE; ++k) {
            uint32_t fresh = 1000 + k;  // never collides with the original keys
            CHECK(m.insert(fresh, (int)k) != nullptr, "churn refill");
        }
        CHECK_EQ(m.size(), CAP, "size after churn refill");
        int visited = 0;
        m.for_each([&](uint32_t, int) { ++visited; });
        CHECK_EQ(visited, CAP, "for_each agrees with size while full");
        // Every permanent key survived this wave of erases and refills.
        for (uint32_t k = 1; k <= PERMANENT; ++k)
            CHECK(m.find(k) && *m.find(k) == (int)k, "permanent key survived the churn");
    }

    // Phase 4: final reachability. The permanent set plus the three refill
    // waves are all present; every churned original is gone.
    for (uint32_t k = 1; k <= PERMANENT; ++k)
        CHECK(m.find(k) && *m.find(k) == (int)k, "permanent key present at the end");
    for (int wave = 0; wave < 3; ++wave) {
        uint32_t lo = PERMANENT + 1 + wave * WAVE;
        for (uint32_t k = lo; k < lo + WAVE; ++k) {
            CHECK(m.find(1000 + k) && *m.find(1000 + k) == (int)k, "refilled key present");
            CHECK(m.find(k) == nullptr, "churned original is gone");
        }
    }
    CHECK_EQ(m.size(), CAP, "still exactly full after the churn");
}

// A full-table erase whose closing scan must run the FULL Capacity-1 bound to
// reach the entry that closes the hole. At Capacity 8, key 5 hashes to slot 0
// and key 13 also hashes to slot 0; with all 8 slots occupied the hole at slot
// 0 can only be closed by sliding key 13 back from slot 7, and reaching slot 7
// takes all 7 scan steps. A scan that stops one step early (the obvious
// off-by-one on `scanned < Capacity - 1`) leaves key 13 sitting at slot 7
// where find(13) — which starts probing at slot 0 and stops at the hole — can
// never reach it. Caught by mutation: this is the assertion that kills it.
static void test_full_table_erase_needs_the_last_scan_step() {
    OpenHashMap<uint32_t, int, 8, 0> m;
    // A full table whose slot 7 holds a key that belongs at slot 0.
    for (uint32_t k : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 13u})
        CHECK(m.insert(k, (int)k) != nullptr, "fill with a wrap-around key");
    CHECK_EQ(m.size(), 8, "table is full");

    CHECK(m.erase(5), "erase the key sitting in slot 0");
    CHECK(m.find(5) == nullptr, "5 is gone");
    CHECK_EQ(m.size(), 7, "size after erase");

    // 13 hashed to slot 0; the shift must pull it back from slot 7, otherwise
    // the hole at slot 0 terminates every later probe for it.
    CHECK(m.find(13) != nullptr, "13 remains findable after a full-table erase");
    CHECK(m.find(13) && *m.find(13) == 13, "13 kept its value");

    // Every other survivor is reachable too.
    for (uint32_t k : {1u, 2u, 3u, 4u, 6u, 7u})
        CHECK(m.find(k) && *m.find(k) == (int)k, "survivor findable");
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
    test_hash_layout_is_what_clusters_assume();
    test_load_factor_boundary();
    test_erase_all_from_full_table();
    test_wraparound_probe_chain_erase();
    test_erase_then_lookup_is_consistent();
    test_iteration_order_is_slot_order_not_insertion_order();
    test_sustained_high_load();
    test_full_table_erase_needs_the_last_scan_step();
    printf("%d/%d OpenHashMap checks passed\n", g_passed, g_tests);
    return g_failed ? 1 : 0;
}
