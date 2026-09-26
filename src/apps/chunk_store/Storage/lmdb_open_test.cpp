// LmdbStore open-path tests (gp-6nmw).
//
// THE BUG. open_() returns void and the constructor ignores it, so a failed
// mdb_env_open leaves the store with a non-null but UNOPENED MDB_env and dbi_
// still 0. Nothing downstream can tell that state apart from a healthy store.
//
// The audit predicted the symptom would be "every writeRaw begins a txn that
// fails, logs 'mdb_txn_begin failed', and returns false". That is wrong, and
// the test asserts the real behaviour: mdb_txn_begin() on a created-but-
// unopened MDB_env dereferences a null map pointer and takes SIGSEGV. So today
// the daemon does not "quietly discard writes" — it crashes the first time it
// touches the store. The contract below is what makes that impossible: open_()
// reports success, the constructor stores it, and every entry point refuses the
// work with one clear message instead of entering a broken LMDB env.
//
// All three open_() sub-defects are addressed by that contract:
//   (a) the discarded mdb_txn_commit of the DBI bootstrap
//   (b) mdb_env_open failure not propagated out of the ctor
//   (c) mdb_dbi_open failure logged but the txn committed anyway
//
// No GoogleTest: this project uses the CHECK/TEST macro harness.

#include "disk/LmdbStore.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/wait.h>
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

// A path that mdb_env_open can never open: the parent directory does not
// exist, so the open fails with ENOENT before any LMDB state is created.
// (A read-only directory and a path that is a regular file were also verified
// to fail, but ENOENT needs no permission games and so is the one CI can rely
// on.)
static std::string unopenablePath() {
    char tmpl[] = "/tmp/lmdb_open_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) return {};
    std::string base(dir);
    std::filesystem::remove_all(base);
    return base + "/no_such_parent_dir/db";
}

// Run `fn` in a forked child so a store that CRASHES on first use is reported
// as a test failure instead of taking the whole harness down with it.
// Returns: 0 = child exited 0, >0 = child exited with that code, <0 = died of
// signal -(-rc).
static int inChild(void (*fn)()) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return -999;
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -999;
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -999;
}

static std::string g_unopenable;

// ---------------------------------------------------------------------------
// Test 1: an unopenable LMDB must be REPORTED, not silently half-constructed
// ---------------------------------------------------------------------------
static void child_writeRaw_on_unopenable() {
    LmdbStore store(g_unopenable, 64ULL * 1024 * 1024);
    std::vector<uint8_t> data(64 * 1024, 0xAB);
    int64_t key = LmdbStore::makeKey(1, 0, 0);
    // A correct store refuses the write and returns false. Today's store
    // reaches mdb_txn_begin() on an unopened env and takes SIGSEGV.
    if (store.writeRaw(key, data.data(), data.size()))
        _exit(2);  // reported success on a store that was never opened
    _exit(1);      // correct: refused, no crash
}

static void test_open_failure_is_reported() {
    printf("TEST 1: open_failure_is_reported\n");

    auto path = unopenablePath();
    CHECK(!path.empty(), "mkdtemp failed");
    g_unopenable = path;

    int rc = inChild(child_writeRaw_on_unopenable);
    CHECK(rc == 1,
          "writeRaw on an unopenable store: child status %d (want 1 = refused, "
          "no crash, no reported success)%s",
          rc,
          rc < 0 ? " — THE STORE CRASHED on a created-but-unopened MDB_env"
                 : (rc == 2 ? " — the store reported a write it could not do"
                            : ""));

    // Destroying a failed store must not touch the missing path either.
    removeDb(path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 2: a store that never opened must reject every read AND write path
// ---------------------------------------------------------------------------
static void child_touch_all_entry_points() {
    LmdbStore store(g_unopenable, 64ULL * 1024 * 1024);
    std::vector<uint8_t> data(4096, 0x11);
    int64_t key = LmdbStore::makeKey(2, 0, 0);

    // writeRaw: must return false.
    if (store.writeRaw(key, data.data(), data.size())) _exit(2);
    // writeBatch: must return false, and must NOT clear the caller's entries
    // (clearing them is how a lost batch gets reported as a saved one).
    std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>> items;
    items.emplace_back(key, std::make_shared<std::vector<uint8_t>>(data));
    if (store.writeBatch(items)) _exit(3);
    if (items.size() != 1) _exit(4);
    // HasChunk: must be false (no chunk), not a crash.
    if (store.HasChunk({2, 0, 0})) _exit(5);
    // readRawBytes: must say "absent" for an absent key without crashing.
    auto r = store.readRawBytes(key);
    if (r.has_value()) _exit(6);
    _exit(1);
}

static void test_unopened_store_rejects_every_entry_point() {
    printf("TEST 2: unopened_store_rejects_every_entry_point\n");

    auto path = unopenablePath();
    CHECK(!path.empty(), "mkdtemp failed");
    g_unopenable = path;

    int rc = inChild(child_touch_all_entry_points);
    CHECK(rc == 1,
          "entry-point sweep on an unopenable store returned %d (want 1): all "
          "read/write entry points must fail fast and uniformly%s",
          rc, rc < 0 ? " — THE STORE CRASHED" : "");

    removeDb(path);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 3: the healthy path is unaffected (the new guard has no false negative)
// ---------------------------------------------------------------------------
static void test_healthy_store_still_works() {
    printf("TEST 3: healthy_store_still_works\n");

    char tmpl[] = "/tmp/lmdb_open_test_ok_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");
    std::string db_path(dir);

    {
        LmdbStore store(db_path, 64ULL * 1024 * 1024);
        std::vector<uint8_t> data(32 * 1024, 0x5A);
        int64_t key = LmdbStore::makeKey(7, 3, 11);
        CHECK(store.writeRaw(key, data.data(), data.size()),
              "writeRaw should succeed on a healthy store");

        auto got = store.readRawBytes(key);
        CHECK(got.has_value(), "the value written should be readable back");
        CHECK(got->size() == data.size() && !got->empty() && (*got)[0] == 0x5A,
              "round-tripped payload differs (size %zu)", got->size());
        CHECK(store.HasChunk({7, 3, 11}), "HasChunk should be true after a write");

        std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>> items;
        items.emplace_back(LmdbStore::makeKey(8, 0, 0),
                           std::make_shared<std::vector<uint8_t>>(16 * 1024, 0x77));
        CHECK(store.writeBatch(items), "writeBatch should succeed on a healthy store");
        CHECK(items.empty(), "a successful writeBatch must clear the caller's items");
    }

    // Reopening the same directory must still work (regression guard: the
    // bootstrap txn in open_() must not be skipped on the second open).
    {
        LmdbStore store(db_path, 64ULL * 1024 * 1024);
        auto got = store.readRawBytes(LmdbStore::makeKey(7, 3, 11));
        CHECK(got.has_value(), "data written by a previous store instance must survive reopen");
        CHECK(store.HasChunk({8, 0, 0}), "the batch-written chunk must survive reopen");
    }

    removeDb(db_path);
    PASS();
}

int main() {
    printf("=== LmdbStore Open-Path Test ===\n\n");

    TEST(open_failure_is_reported);
    TEST(unopened_store_rejects_every_entry_point);
    TEST(healthy_store_still_works);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
