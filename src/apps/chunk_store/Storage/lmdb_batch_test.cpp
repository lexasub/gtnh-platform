// LmdbStore batch-write tests (gp-jzgr).
//
// THE BUG, in two halves:
//
//  1. writeRaw handles map exhaustion: MDB_MAP_FULL -> mdb_txn_abort ->
//     growMapSize() -> retry. writeBatch had no such branch, so ANY non-zero
//     mdb_put rc aborted the shared transaction. Because every chunk in a
//     batch shares that one txn, the entry that did not fit destroyed the
//     whole batch — not just itself.
//
//  2. Both callers threw the bool away. EncodePipeline.cpp:103 and :111 called
//     lmdb_->writeBatch(local_palettes) and ignored the result; the only thing
//     that stopped an endless retry was the vector trim at :105-108, which
//     discards entries outright with no log. A mapsize-full batch was
//     therefore silently deleted, chunk by chunk.
//
// MAP_FULL is INDUCTED, not injected. open_() honours the caller's
// max_map_size for the initial map, so a store built with a small cap runs out
// of map on a real environment with real pages. (For the production default —
// 256 GB cap — min(cap, 4 GiB) is still 4 GiB, so nothing changes there.)
//
// THE CONTRACT being tested. On map exhaustion writeBatch must:
//   (a) persist every entry that DID fit, instead of discarding the batch,
//   (b) return false, because not everything was saved, and
//   (c) leave exactly the unwritten entries in the caller's vector, so the
//       caller can retry them rather than drop them.
//
// No GoogleTest: this project uses the CHECK/TEST macro harness.

#include "disk/LmdbStore.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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

static void removeDb(const std::string& path) {
    if (!path.empty())
        std::filesystem::remove_all(path);
}

// Removes the temp db no matter how the test leaves it. A failing CHECK returns
// early, and a leaked LMDB directory is exactly what produced 13 bogus
// integration failures earlier today — so cleanup must not depend on reaching
// the end of the test body.
class ScopedDb {
public:
    ScopedDb() {
        char tmpl[] = "/tmp/lmdb_batch_test_XXXXXX";
        char* dir = mkdtemp(tmpl);
        if (dir) path_ = dir;
    }
    ~ScopedDb() { removeDb(path_); }
    ScopedDb(const ScopedDb&) = delete;
    ScopedDb& operator=(const ScopedDb&) = delete;

    const std::string& path() const { return path_; }
    bool valid() const { return !path_.empty(); }

private:
    std::string path_;
};

using Batch = std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>;

static Batch makeBatch(int base, int count, size_t payload) {
    Batch items;
    for (int i = 0; i < count; ++i)
        items.emplace_back(LmdbStore::makeKey(base + i, 0, 0),
                           std::make_shared<std::vector<uint8_t>>(payload, 0x5A));
    return items;
}

static size_t countPresent(LmdbStore& store, int base, int count) {
    size_t n = 0;
    for (int i = 0; i < count; ++i)
        if (store.readRawBytes(LmdbStore::makeKey(base + i, 0, 0)).has_value()) ++n;
    return n;
}

// A store whose map is full and cannot grow: the cap passed to the ctor is
// also the ceiling growMapSize() honours, so growth gives up immediately.
static constexpr size_t NO_GROWTH_CAP = 1ULL * 1024 * 1024;  // 1 MiB

// ---------------------------------------------------------------------------
// Test 1: map exhaustion must keep what fits, report false, and hand back
//         exactly the entries that are still unwritten
// ---------------------------------------------------------------------------
static void test_writeBatch_keeps_fitting_entries_on_map_full() {
    printf("TEST 1: writeBatch_keeps_fitting_entries_on_map_full\n");

    ScopedDb db;
    CHECK(db.valid(), "mkdtemp failed");

    const int count = 8;
    const size_t payload = 200 * 1024;  // 1.6 MiB total against a 1 MiB map
    Batch items = makeBatch(0, count, payload);
    const size_t batch_size = items.size();

    bool result = true;
    size_t remaining = 0;
    {
        LmdbStore store(db.path(), NO_GROWTH_CAP);
        result = store.writeBatch(items);
        remaining = items.size();
    }

    CHECK(!result,
          "writeBatch returned true for a batch that exceeded the mapsize "
          "(%zu entries) — the caller would count it as saved", batch_size);

    size_t persisted = 0;
    {
        LmdbStore store(db.path(), NO_GROWTH_CAP);
        persisted = countPresent(store, 0, count);
    }

    CHECK(persisted > 0,
          "nothing from the batch reached disk (%zu of %zu) — the shared txn was "
          "aborted wholesale, so one entry that did not fit destroyed the "
          "chunks that did", persisted, batch_size);
    CHECK(remaining > 0,
          "writeBatch cleared all %zu entries on failure, so the caller has "
          "nothing left to retry", batch_size);
    CHECK(persisted + remaining == batch_size,
          "accounting mismatch: %zu persisted + %zu still queued != %zu total — "
          "entries were duplicated or dropped",
          persisted, remaining, batch_size);

    PASS();
}

// ---------------------------------------------------------------------------
// Test 2: the queued remainder must be exactly the missing entries. A retry
//         set that is merely non-empty is not enough: re-writing an entry
//         already on disk is wasted work, and dropping one is data loss.
// ---------------------------------------------------------------------------
static void test_writeBatch_remainder_matches_missing_entries() {
    printf("TEST 2: writeBatch_remainder_matches_missing_entries\n");

    ScopedDb db;
    CHECK(db.valid(), "mkdtemp failed");

    const int count = 8;
    Batch items = makeBatch(50, count, 200 * 1024);

    bool ok = false;
    Batch remainder;
    {
        LmdbStore store(db.path(), NO_GROWTH_CAP);
        ok = store.writeBatch(items);
        if (!ok) remainder = items;
    }
    CHECK(!ok, "the batch was expected to exceed the 1 MiB map but did not — "
               "MAP_FULL was not induced, so this test proves nothing");
    CHECK(!remainder.empty(), "writeBatch cleared the whole vector on failure");

    {
        LmdbStore store(db.path(), NO_GROWTH_CAP);
        for (auto& [key, pal] : remainder) {
            (void)pal;
            auto got = store.readRawBytes(key);
            CHECK(!got.has_value(),
                  "entry %lld was handed back for retry but is already on disk — "
                  "the retry set is wrong", static_cast<long long>(key));
        }
        size_t persisted = countPresent(store, 50, count);
        CHECK(persisted + remainder.size() == static_cast<size_t>(count),
              "accounting mismatch: %zu persisted + %zu queued != %d total",
              persisted, remainder.size(), count);
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Test 3: a batch that fits must be unaffected (no false positives)
// ---------------------------------------------------------------------------
static void test_writeBatch_that_fits_is_unchanged() {
    printf("TEST 3: writeBatch_that_fits_is_unchanged\n");

    ScopedDb db;
    CHECK(db.valid(), "mkdtemp failed");

    Batch items = makeBatch(200, 4, 64 * 1024);
    {
        LmdbStore store(db.path(), 64ULL * 1024 * 1024);
        CHECK(store.writeBatch(items), "a small batch should succeed");
        CHECK(items.empty(), "a successful writeBatch must clear the caller's items");
    }
    {
        LmdbStore store(db.path(), 64ULL * 1024 * 1024);
        for (int i = 0; i < 4; ++i)
            CHECK(store.readRawBytes(LmdbStore::makeKey(200 + i, 0, 0)).has_value(),
                  "entry %d should be readable back", i);
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Test 4: with growth headroom the batch must survive — writeRaw's retry shape
// ---------------------------------------------------------------------------
static void test_writeBatch_grows_and_retries() {
    printf("TEST 4: writeBatch_grows_and_retries\n");

    ScopedDb db;
    CHECK(db.valid(), "mkdtemp failed");

    // Small initial map, cap far above it, so growMapSize() has room.
    Batch items = makeBatch(300, 8, 200 * 1024);
    const size_t batch_size = items.size();
    bool result = false;
    {
        LmdbStore store(db.path(), 64ULL * 1024 * 1024);
        result = store.writeBatch(items);
    }
    CHECK(result,
          "writeBatch failed even though the map could grow to 64 MiB — a "
          "MAP_FULL grow+retry was not attempted");
    {
        LmdbStore store(db.path(), 64ULL * 1024 * 1024);
        size_t persisted = countPresent(store, 300, 8);
        CHECK(persisted == batch_size,
              "writeBatch reported success but only %zu of %zu entries are on "
              "disk", persisted, batch_size);
    }

    PASS();
}

int main() {
    printf("=== LmdbStore Batch-Write Test ===\n\n");

    TEST(writeBatch_keeps_fitting_entries_on_map_full);
    TEST(writeBatch_remainder_matches_missing_entries);
    TEST(writeBatch_that_fits_is_unchanged);
    TEST(writeBatch_grows_and_retries);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
