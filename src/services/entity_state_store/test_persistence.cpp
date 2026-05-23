#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

#include <unistd.h>
#include <asio/io_context.hpp>
#include "EntityStateStorage.h"

static int g_tests = 0, g_passed = 0, g_failed = 0;

static void test_check(bool cond, const char* file, int line, const char* expr, const char* msg) {
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
#define PASS() do { ++g_passed; } while(0)

static std::string makeTempDb() {
    char tmpl[] = "/tmp/entity_state_test_XXXXXX.mdb";
    int fd = mkstemps(tmpl, 4);
    if (fd == -1) { perror("mkstemps"); exit(1); }
    close(fd);
    return std::string(tmpl);
}

static void removeDb(const std::string& path) {
    std::string cmd = "rm -f " + path;
    system(cmd.c_str());
}

static void test_save_and_load() {
    auto db_path = makeTempDb();
    asio::io_context io;
    EntityStateStorage storage(db_path, io);
    CHECK(storage.initialize(), "initialize succeeds");

    std::vector<uint8_t> saved = {'H', 'e', 'l', 'l', 'o', 0};
    CHECK(storage.SaveEntityState(0, 10, 20, 30, 1, saved), "save succeeds");

    std::vector<uint8_t> loaded;
    CHECK(storage.LoadEntityState(0, 10, 20, 30, 1, loaded), "load succeeds");
    CHECK_EQ(loaded.size(), saved.size(), "loaded size matches saved");
    CHECK(loaded.size() == 0 || loaded[0] == 'H', "first byte matches");

    storage.shutdown();
    removeDb(db_path);
    PASS();
}

static void test_load_missing() {
    auto db_path = makeTempDb();
    asio::io_context io;
    EntityStateStorage storage(db_path, io);
    CHECK(storage.initialize(), "initialize succeeds");

    std::vector<uint8_t> loaded;
    CHECK(!storage.LoadEntityState(0, 99, 99, 99, 1, loaded), "load missing returns false");

    storage.shutdown();
    removeDb(db_path);
    PASS();
}

static void test_overwrite() {
    auto db_path = makeTempDb();
    asio::io_context io;
    EntityStateStorage storage(db_path, io);
    CHECK(storage.initialize(), "initialize succeeds");

    std::vector<uint8_t> first = {1, 2, 3};
    std::vector<uint8_t> second = {4, 5, 6, 7};
    CHECK(storage.SaveEntityState(0, 10, 20, 30, 1, first), "first save");
    CHECK(storage.SaveEntityState(0, 10, 20, 30, 1, second), "second save");

    std::vector<uint8_t> loaded;
    CHECK(storage.LoadEntityState(0, 10, 20, 30, 1, loaded), "load after overwrite");
    CHECK_EQ(loaded.size(), second.size(), "loaded matches second write");

    storage.shutdown();
    removeDb(db_path);
    PASS();
}

static void test_delete() {
    auto db_path = makeTempDb();
    asio::io_context io;
    EntityStateStorage storage(db_path, io);
    CHECK(storage.initialize(), "initialize succeeds");

    std::vector<uint8_t> data = {42};
    CHECK(storage.SaveEntityState(0, 10, 20, 30, 1, data), "save");
    CHECK(storage.DeleteEntityState(0, 10, 20, 30, 1), "delete succeeds");

    std::vector<uint8_t> loaded;
    CHECK(!storage.LoadEntityState(0, 10, 20, 30, 1, loaded), "load after delete fails");

    storage.shutdown();
    removeDb(db_path);
    PASS();
}

static void test_multiple_entities() {
    auto db_path = makeTempDb();
    asio::io_context io;
    EntityStateStorage storage(db_path, io);
    CHECK(storage.initialize(), "initialize succeeds");

    std::vector<uint8_t> stateA = {10};
    std::vector<uint8_t> stateB = {20};
    std::vector<uint8_t> stateC = {30};
    CHECK(storage.SaveEntityState(0, 1, 2, 3, 1, stateA), "save A");
    CHECK(storage.SaveEntityState(0, 4, 5, 6, 1, stateB), "save B");
    CHECK(storage.SaveEntityState(1, 1, 2, 3, 2, stateC), "save C (different dim+type)");

    std::vector<uint8_t> loaded;
    CHECK(storage.LoadEntityState(0, 1, 2, 3, 1, loaded), "load A");
    CHECK_EQ(loaded[0], 10, "A value correct");
    CHECK(storage.LoadEntityState(0, 4, 5, 6, 1, loaded), "load B");
    CHECK_EQ(loaded[0], 20, "B value correct");
    CHECK(storage.LoadEntityState(1, 1, 2, 3, 2, loaded), "load C");
    CHECK_EQ(loaded[0], 30, "C value correct");

    storage.shutdown();
    removeDb(db_path);
    PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

int main(int, char**) {
    printf("=== EntityStateStore Persistence Test ===\n\n");
    TEST(save_and_load);
    TEST(load_missing);
    TEST(overwrite);
    TEST(delete);
    TEST(multiple_entities);
    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
