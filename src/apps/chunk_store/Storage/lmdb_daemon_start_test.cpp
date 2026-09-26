// Daemon-startup contract test (gp-6nmw, second half).
//
// The decision recorded in the fix is: an unopenable LMDB is a STARTUP
// FAILURE. ChunkStore exposes lmdb().isOpen(); main.cpp checks it and exits 1
// without ever binding a port. This test drives the real chunkd binary against
// a genuinely unopenable db path and asserts it dies with a non-zero status
// and a message naming the database, rather than reaching the "listening" or
// "connected" stage that used to be reachable with a dead store.
//
// It also asserts the converse, so a broken path is not "fixed" by always
// exiting 1: a HEALTHY path must get past the store check (it will then fail
// later, on the router connect, because no MessageRouter is running here — that
// is a different, correctly-reported failure).
//
// No network, no ports, no cluster: the binary is pointed at an unopenable
// directory and its output/exit status inspected. Runs the binary as a
// subprocess, so nothing here depends on chunkd's internals.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

struct RunResult {
    int exit_code = 0;       // -signal if it died
    std::string output;
};

// The chunkd binary under test, supplied as argv[1].
static std::string g_chunkd;

// Run argv to completion with a bounded timeout, capturing merged output.
// timeout(1) so a wedged daemon cannot hang the suite.
static RunResult runBounded(const std::vector<std::string>& args) {
    RunResult res;
    std::string cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd += ' ';
        cmd += '\'';
        for (char c : args[i]) {
            if (c == '\'') cmd += "'\\''";
            else cmd += c;
        }
        cmd += '\'';
    }
    cmd += " 2>&1";

    std::string out_path = "/tmp/lmdb_daemon_start_test_out.txt";
    std::string full = "timeout -k 2 20 " + cmd + " > " + out_path + " 2>&1";
    int rc = std::system(full.c_str());
    if (rc == -1) { res.exit_code = -999; return res; }
    if (WIFSIGNALED(rc)) res.exit_code = -WTERMSIG(rc);
    else res.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -998;

    std::FILE* f = std::fopen(out_path.c_str(), "rb");
    if (f) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            res.output.append(buf, n);
        std::fclose(f);
    }
    std::filesystem::remove(out_path);
    return res;
}

// ---------------------------------------------------------------------------
// Test 1: chunkd on an unopenable db path must exit non-zero and say why
// ---------------------------------------------------------------------------
static void test_daemon_refuses_to_start_on_unopenable_db() {
    printf("TEST 1: daemon_refuses_to_start_on_unopenable_db\n");

    char tmpl[] = "/tmp/lmdb_daemon_start_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");
    std::string base(dir);
    std::filesystem::remove_all(base);
    std::string bad_db = base + "/no_such_parent_dir/db";

    // Port 0 / an unbindable port keeps this test hermetic: the store check
    // must fire BEFORE any socket is created, so the port must never be used.
    auto res = runBounded({g_chunkd, bad_db, "0"});

    printf("---- chunkd output ----\n%s--------------------\n", res.output.c_str());

    CHECK(res.exit_code != 0,
          "chunkd exited 0 on an unopenable db path (output: %s) — the daemon "
          "must refuse to start", res.output.c_str());
    CHECK(res.output.find("could not be opened") != std::string::npos,
          "chunkd's output does not explain the failure; it must name the "
          "unopenable database (output: %s)", res.output.c_str());
    CHECK(res.output.find("refusing to start") != std::string::npos,
          "chunkd did not state that it is refusing to start (output: %s)",
          res.output.c_str());
    // The whole point: it must never reach the "serving" stage.
    CHECK(res.output.find("listening") == std::string::npos,
          "chunkd announced a listener on an unopenable database — this is the "
          "gp-6nmw failure mode (output: %s)", res.output.c_str());

    std::filesystem::remove_all(base);
    PASS();
}

// ---------------------------------------------------------------------------
// Test 2: a HEALTHY db path must get past the store check
// ---------------------------------------------------------------------------
static void test_daemon_starts_on_healthy_db() {
    printf("TEST 2: daemon_starts_on_healthy_db\n");

    char tmpl[] = "/tmp/lmdb_daemon_ok_XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "mkdtemp failed");
    std::string db_path(dir);

    // It will still exit (no MessageRouter to connect to in this hermetic
    // test, and timeout(1) bounds it), but it must NOT fail the store check.
    auto res = runBounded({g_chunkd, db_path, "0"});
    printf("---- chunkd output ----\n%s--------------------\n", res.output.c_str());

    CHECK(res.output.find("could not be opened") == std::string::npos,
          "chunkd rejected a perfectly good db path (output: %s)",
          res.output.c_str());
    CHECK(res.output.find("refusing to start") == std::string::npos,
          "chunkd refused to start on a healthy db path (output: %s)",
          res.output.c_str());
    CHECK(res.output.find("listening") != std::string::npos,
          "chunkd never reached the listener on a healthy db path, so test 1 "
          "passes for the wrong reason (output: %s)", res.output.c_str());

    std::filesystem::remove_all(db_path);
    PASS();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/chunkd\n", argv[0]);
        return 2;
    }
    g_chunkd = argv[1];

    printf("=== ChunkStore Daemon Startup Contract Test ===\n");
    printf("chunkd under test: %s\n\n", g_chunkd.c_str());

    // A healthy path reaches the listener and then fails to reach a
    // MessageRouter (none is running here); timeout(1) bounds that wait. An
    // unopenable path must die earlier, at the store check.
    TEST(daemon_refuses_to_start_on_unopenable_db);
    TEST(daemon_starts_on_healthy_db);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
