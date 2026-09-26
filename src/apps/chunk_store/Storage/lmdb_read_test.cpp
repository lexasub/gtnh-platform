// ChunkStore read-failure tests (gp-wjmb).
//
// THE BUG. LmdbStore::readRawBytes returned std::nullopt for BOTH
// MDB_NOTFOUND and a genuine read error, so a caller could not tell "this chunk
// is not stored" from "the disk did not answer". ChunkStore::setBlock read that
// as absence: it built a fresh MutableChunk, set the one block, and cached +
// markDirtyed it. The next flush wrote back a chunk that was entirely air
// except the single block just placed — one read error destroyed 192 KB of
// terrain.
//
// A GENUINE read error is induced, not injected at a seam. LMDB keeps a fixed
// reader table (maxreaders slots, 1024 here) and mdb_txn_begin on a read txn
// takes a slot from it. A second MDB_env opened on the same database shares
// that table, so holding 1024 read transactions open from the helper env makes
// the store's own read begin fail with MDB_READERS_FULL — a real error from a
// real LMDB call, fully recovered once the slots are released.
//
// The tests below use only the PRE-FIX API on purpose: the defect is that a
// read error is indistinguishable from absence, so the only way to witness it
// through today's interface is to observe the damage it does. The API-level
// distinction is asserted by the tests added alongside the fix.
//
// No GoogleTest: this project uses the CHECK/TEST macro harness.

#include "ChunkStore.h"
#include "cache/MutableChunk.h"
#include "disk/LmdbStore.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <lmdb.h>
#include <memory>
#include <string>
#include <unistd.h>
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

// Removes the temp db however the test leaves it. A failing CHECK returns
// early, and a leaked LMDB directory is what produced 13 bogus integration
// failures earlier today, so cleanup cannot depend on reaching the end.
class ScopedDb {
public:
    ScopedDb() {
        char tmpl[] = "/tmp/lmdb_read_test_XXXXXX";
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

// Holds the shared LMDB reader table full, so the store's next read fails with
// MDB_READERS_FULL. RAII: the slots are released however the test leaves it,
// including on an early CHECK return.
class ReaderTableExhaustion {
public:
    explicit ReaderTableExhaustion(const std::string& path) {
        // maxreaders must match the store's, or mdb_env_open rejects the
        // second env on the same lock file.
        if (mdb_env_create(&env_) != 0) return;
        mdb_env_set_maxdbs(env_, 1024);
        mdb_env_set_maxreaders(env_, 1024);
        if (mdb_env_open(env_, path.c_str(), MDB_NOTLS, 0664) != 0) {
            mdb_env_close(env_);
            env_ = nullptr;
            return;
        }
        MDB_txn* txn = nullptr;
        if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) == 0)
            mdb_txn_abort(txn);
        for (int i = 0; i < 4096; ++i) {
            MDB_txn* t = nullptr;
            if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &t) != 0) break;
            held_.push_back(t);
        }
    }
    ~ReaderTableExhaustion() {
        for (MDB_txn* t : held_) mdb_txn_abort(t);
        held_.clear();
        if (env_) mdb_env_close(env_);
    }
    ReaderTableExhaustion(const ReaderTableExhaustion&) = delete;
    ReaderTableExhaustion& operator=(const ReaderTableExhaustion&) = delete;

    size_t held() const { return held_.size(); }

private:
    MDB_env* env_ = nullptr;
    std::vector<MDB_txn*> held_;
};

// ---------------------------------------------------------------------------
// Test 1: a read that cannot be performed must not lead to overwriting the
//         stored chunk
// ---------------------------------------------------------------------------
static void test_setBlock_does_not_overwrite_on_read_error() {
    printf("TEST 1: setBlock_does_not_overwrite_on_read_error\n");

    ScopedDb db;
    CHECK(db.valid(), "mkdtemp failed");

    // Seed a chunk with several recognisable blocks and flush it to LMDB, so
    // the next store must read it back from disk.
    {
        ChunkStore store(db.path());
        store.setBlock(5, 7, 9, 1234, 5);
        store.setBlock(6, 7, 9, 2345, 6);
        store.setBlock(5, 8, 9, 3456, 7);
        CHECK(store.flushDirtyChunks(), "seeding flush should succeed");
    }

    // Confirm the seed really is on disk before testing anything.
    {
        ChunkStore store(db.path());
        CHECK(store.GetBlockAt({5, 7, 9}) == 1234,
              "the seeded chunk did not survive the reopen (block=%u)",
              store.GetBlockAt({5, 7, 9}));
    }

    // The store is constructed BEFORE the reader table is exhausted. This
    // ordering matters: mdb_env_open touches the lock file, and opening a
    // second env while the shared reader table is already full can re-initialise
    // it — which would silently un-starve the store and make this test prove
    // nothing. (Same discipline as lmdb_commit_test.cpp with RLIMIT_FSIZE.)
    bool set_result = true;
    {
        ChunkStore store(db.path());
        {
            ReaderTableExhaustion starve(db.path());
            CHECK(starve.held() > 0,
                  "could not exhaust the LMDB reader table — the read error was "
                  "never induced, so this test proves nothing");
            set_result = store.setBlock(6, 7, 9, 9999, 9);
        }
        // The store's shutdown flush runs here, with reads working again. If
        // setBlock wrongly cached an all-air chunk, that flush is what lands it
        // on disk — which is exactly the corruption being tested for.
    }

    CHECK(!set_result,
          "setBlock reported success while the chunk could not be read — the "
          "near-empty replacement chunk was cached and would be flushed over "
          "192 KB of real terrain");

    // The stored chunk must be intact. A fresh store reads it from disk with
    // no cache and no starvation.
    {
        ChunkStore store(db.path());
        uint16_t b_target = store.GetBlockAt({6, 7, 9});
        uint16_t b_neighbour = store.GetBlockAt({5, 8, 9});
        uint8_t m_target = store.GetMeta(6, 7, 9);

        CHECK(b_target == 2345,
              "block (6,7,9) is now %u, want 2345 — the failed read replaced the "
              "chunk instead of reporting the failure", b_target);
        CHECK(m_target == 6, "meta (6,7,9) is now %u, want 6", m_target);
        CHECK(b_neighbour == 3456,
              "neighbouring block (5,8,9) is now %u, want 3456 — the chunk was "
              "replaced with air plus the one block just set", b_neighbour);
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Test 2: a read failure must not be reported to a caller as "chunk absent"
//         (GetChunk must not silently hand back a fresh empty chunk)
// ---------------------------------------------------------------------------
static void test_getChunk_does_not_invent_a_chunk_on_read_error() {
    printf("TEST 2: getChunk_does_not_invent_a_chunk_on_read_error\n");

    ScopedDb db;
    CHECK(db.valid(), "mkdtemp failed");

    {
        ChunkStore store(db.path());
        store.setBlock(40, 40, 40, 777, 3);
        CHECK(store.flushDirtyChunks(), "seeding flush should succeed");
    }

    // Construct the store BEFORE starving the reader table — see the note in
    // test 1; opening an env while the table is full can reset it.
    {
        ChunkStore store(db.path());
        {
            ReaderTableExhaustion starve(db.path());
            CHECK(starve.held() > 0,
                  "could not exhaust the reader table — the read error was never "
                  "induced, so this test proves nothing");

            // GetChunk on a chunk that exists on disk. If the read failure is
            // read as "absent", this returns a fresh all-air chunk, which the
            // caller cannot tell apart from a genuinely new chunk. The fix must
            // report the failure instead (nullptr).
            const MutableChunk* c = store.GetChunk({1, 1, 1});
            CHECK(c == nullptr,
                  "GetChunk returned a chunk while the read could not be "
                  "performed — a read error is being presented as 'no chunk "
                  "stored'");
        }
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Test 3: the three read outcomes are distinct at the API level. This is the
//         contract the fix introduces; the tests above witness its absence
//         through the damage it caused.
// ---------------------------------------------------------------------------
static void test_readResult_distinguishes_absent_from_failed() {
    printf("TEST 3: readResult_distinguishes_absent_from_failed\n");

    ScopedDb db;
    CHECK(db.valid(), "mkdtemp failed");

    {
        LmdbStore store(db.path(), 64ULL * 1024 * 1024);
        std::vector<uint8_t> data(32 * 1024, 0x3B);
        int64_t key = LmdbStore::makeKey(3, 3, 3);
        CHECK(store.writeRaw(key, data.data(), data.size()),
              "seeding writeRaw should succeed");
    }

    LmdbStore store(db.path(), 64ULL * 1024 * 1024);
    const int64_t present = LmdbStore::makeKey(3, 3, 3);
    const int64_t absent  = LmdbStore::makeKey(4, 4, 4);

    // Healthy reads: one value, one miss — and they must not look alike.
    {
        auto got = store.readRawBytes(present);
        CHECK(got.has_value(), "the seeded key should read back");
        CHECK(got->size() == 32 * 1024, "seeded payload size is %zu, want %d",
              got->size(), 32 * 1024);
        auto miss = store.readRawBytes(absent);
        CHECK(!miss.has_value(), "an unseeded key should read as absent");
        CHECK(miss.error() == ReadError::NotFound,
              "a genuine miss must report ReadError::NotFound, got %d",
              static_cast<int>(miss.error()));
    }

    // Reads failing: BOTH keys must now report Failed, never NotFound.
    {
        ReaderTableExhaustion starve(db.path());
        CHECK(starve.held() > 0,
              "could not exhaust the reader table — the read error was never "
              "induced, so this test proves nothing");

        auto present_failed = store.readRawBytes(present);
        auto absent_failed = store.readRawBytes(absent);

        CHECK(!present_failed.has_value() && present_failed.error() == ReadError::Failed,
              "a read of a PRESENT key reported error %d (want Failed=%d) while "
              "the reader table was full",
              static_cast<int>(present_failed.error()),
              static_cast<int>(ReadError::Failed));
        CHECK(absent_failed.error() == ReadError::Failed,
              "a read of an ABSENT key reported error %d (want Failed=%d) — a "
              "read error is still being reported as absence",
              static_cast<int>(absent_failed.error()),
              static_cast<int>(ReadError::Failed));
    }

    PASS();
}

// ---------------------------------------------------------------------------
// Test 4: once reads work again everything behaves normally — the refusal must
//         not be sticky.
// ---------------------------------------------------------------------------
static void test_recovers_after_read_failures_stop() {
    printf("TEST 4: recovers_after_read_failures_stop\n");

    ScopedDb db;
    CHECK(db.valid(), "mkdtemp failed");

    {
        LmdbStore store(db.path(), 64ULL * 1024 * 1024);
        std::vector<uint8_t> data(16 * 1024, 0x2C);
        int64_t key = LmdbStore::makeKey(9, 9, 9);
        CHECK(store.writeRaw(key, data.data(), data.size()),
              "seeding should succeed");
    }

    LmdbStore store(db.path(), 64ULL * 1024 * 1024);
    {
        ReaderTableExhaustion starve(db.path());
        CHECK(starve.held() > 0, "could not exhaust the reader table");
        auto failed = store.readRawBytes(LmdbStore::makeKey(9, 9, 9));
        CHECK(!failed.has_value() && failed.error() == ReadError::Failed,
              "expected ReadError::Failed while the reader table was full, got %d",
              static_cast<int>(failed.error()));
    }
    // ReaderTableExhaustion went out of scope: the slots are released.
    {
        auto ok = store.readRawBytes(LmdbStore::makeKey(9, 9, 9));
        CHECK(ok.has_value(),
              "the store did not recover after the reader table was released "
              "(error=%d)", static_cast<int>(ok.error()));
        auto miss = store.readRawBytes(LmdbStore::makeKey(8, 8, 8));
        CHECK(!miss.has_value(), "an unseeded key should read as absent again");
        CHECK(miss.error() == ReadError::NotFound,
              "a genuine miss must read as NotFound after recovery, got %d",
              static_cast<int>(miss.error()));
    }

    // And writes still work after the failed reads.
    {
        std::vector<uint8_t> data(8 * 1024, 0x1D);
        int64_t key = LmdbStore::makeKey(7, 7, 7);
        CHECK(store.writeRaw(key, data.data(), data.size()),
              "writes should still succeed after a run of failed reads");
    }

    PASS();
}

int main() {
    printf("=== ChunkStore Read-Failure Test ===\n\n");

    TEST(setBlock_does_not_overwrite_on_read_error);
    TEST(getChunk_does_not_invent_a_chunk_on_read_error);
    TEST(readResult_distinguishes_absent_from_failed);
    TEST(recovers_after_read_failures_stop);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
