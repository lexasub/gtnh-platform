// LmdbStore commit-failure tests (gp-q4aa).
//
// The bug: LMDB_TX_COMMIT() logged a failed mdb_txn_commit and fell through, so
// both writeRaw() and writeBatch() returned true for a write that never reached
// disk. The flush thread then counted the chunk as saved and the client got a
// success SaveChunkResp.
//
// The commit failure is INDUCTED, not injected at a seam. RLIMIT_FSIZE caps the
// length of any pwrite(). LMDB flushes its dirty pages with pwrite() inside
// mdb_page_flush(), which _mdb_txn_commit() propagates out of mdb_txn_commit().
// With the limit set below the size of one page, every commit that actually has
// pages to write fails with EFBIG — a real error from a real syscall on a real
// LMDB environment. RLIMIT_FSIZE is per-process and the original is restored
// after every test, so no other test in this binary is affected.
//
// No GoogleTest: this project uses the CHECK/TEST macro harness, the same shape
// as the other chunk_store tests.

#include "disk/LmdbStore.h"
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/resource.h>
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

// Unique temp dir per test, always removed — a killed run must not leak an LMDB
// cluster, which is what caused 13 bogus integration failures earlier today.
static std::string makeTempDb() {
    char tmpl[] = "/tmp/lmdb_commit_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

static void removeDb(const std::string& path) {
    if (!path.empty())
        std::filesystem::remove_all(path);
}

// Scoped RLIMIT_FSIZE cap. RAII so a failing CHECK can never leave the limit
// lowered for the tests that follow it.
class FileSizeLimit {
public:
    explicit FileSizeLimit(rlim_t limit) {
        if (getrlimit(RLIMIT_FSIZE, &saved_) != 0)
            saved_ = {0, 0};
        rlim_t next = limit;
        if (next > saved_.rlim_cur)
            next = saved_.rlim_cur;
        struct rlimit rl = saved_;
        rl.rlim_cur = next;
        if (setrlimit(RLIMIT_FSIZE, &rl) != 0)
            active_ = false;
    }
    ~FileSizeLimit() {
        if (active_)
            setrlimit(RLIMIT_FSIZE, &saved_);
    }
    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;

private:
    struct rlimit saved_{};
    bool active_ = true;
};

static std::vector<uint8_t> makeChunk(size_t size, uint8_t fill) {
    return std::vector<uint8_t>(size, fill);
}

// ---------------------------------------------------------------------------
// Test 1: writeRaw must return false when mdb_txn_commit fails
// ---------------------------------------------------------------------------
static void test_writeRaw_reports_commit_failure() {
    printf("TEST 1: writeRaw_reports_commit_failure\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    // Reference: an unlimited write of the same payload commits fine, so a
    // false below can only come from the commit, not from the put.
    {
        LmdbStore store(db_path, 1ULL * 1024 * 1024 * 1024);
        auto data = makeChunk(64 * 1024, 0xAB);
        int64_t key = LmdbStore::makeKey(1, 0, 0);
        CHECK(store.writeRaw(key, data.data(), data.size()),
              "baseline writeRaw with no size cap should succeed");
        CHECK(store.readRawBytes(key).has_value(), "baseline value should be readable");
    }

    // Now cap writes below one page and write a fresh key. mdb_put still
    // succeeds (it only touches the mmap), but the commit's pwrite cannot.
    // The store is constructed BEFORE the limit is applied: mdb_env_open
    // writes the lock file and meta page, which would fail first and mask the
    // commit we are actually trying to break.
    bool result = true;
    {
        LmdbStore store(db_path, 1ULL * 1024 * 1024 * 1024);
        auto data = makeChunk(64 * 1024, 0xCD);
        int64_t key = LmdbStore::makeKey(2, 0, 0);
        {
            FileSizeLimit limit(1);
            result = store.writeRaw(key, data.data(), data.size());
        }
    }

    CHECK(!result,
          "writeRaw returned true after mdb_txn_commit failed — the write was lost "
          "but reported as saved");

    // The value really is gone: not merely unreported, actually absent.
    {
        LmdbStore store(db_path, 1ULL * 1024 * 1024 * 1024);
        int64_t key = LmdbStore::makeKey(2, 0, 0);
        CHECK(!store.readRawBytes(key).has_value(),
              "value should not be on disk after the failed commit");
    }

    removeDb(db_path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 2: writeBatch must return false when mdb_txn_commit fails
// ---------------------------------------------------------------------------
static void test_writeBatch_reports_commit_failure() {
    printf("TEST 2: writeBatch_reports_commit_failure\n");

    auto db_path = makeTempDb();
    CHECK(!db_path.empty(), "mkdtemp failed");

    std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>> items;
    for (int i = 0; i < 4; ++i) {
        items.emplace_back(LmdbStore::makeKey(10 + i, 0, 0),
                           std::make_shared<std::vector<uint8_t>>(64 * 1024, 0xEF));
    }
    const size_t batch_size = items.size();

    bool result = true;
    size_t remaining = 0;
    {
        LmdbStore store(db_path, 1ULL * 1024 * 1024 * 1024);
        {
            FileSizeLimit limit(1);
            result = store.writeBatch(items);
            remaining = items.size();
        }
    }

    CHECK(!result,
          "writeBatch returned true after mdb_txn_commit failed — every chunk in "
          "the batch was lost but the batch was reported as saved");
    // items.clear() must not run on the failure path: the caller needs the
    // entries back to retry or re-queue them.
    CHECK(remaining == batch_size,
          "writeBatch cleared items on the failure path (%zu left, want %zu)",
          remaining, batch_size);

    {
        LmdbStore store(db_path, 1ULL * 1024 * 1024 * 1024);
        for (int i = 0; i < 4; ++i)
            CHECK(!store.readRawBytes(LmdbStore::makeKey(10 + i, 0, 0)).has_value(),
                  "batch value %d should not be on disk after the failed commit", i);
    }

    removeDb(db_path);
    PASS();
}

int main() {
    // A pwrite() past RLIMIT_FSIZE raises SIGXFSZ, whose default action kills
    // the process. Ignore it so pwrite() returns EFBIG and LMDB can report it.
    signal(SIGXFSZ, SIG_IGN);

    printf("=== LmdbStore Commit-Failure Test ===\n\n");

    TEST(writeRaw_reports_commit_failure);
    TEST(writeBatch_reports_commit_failure);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
