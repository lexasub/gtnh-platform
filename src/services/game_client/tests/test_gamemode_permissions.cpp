// GameModePerm permission matrix tests (game-modes capability).
// Verifies canFly/noclip/canBreak/canPlace/infiniteItems per GameMode against
// the enforced client behavior:
//
//   | Mode      | canFly | noclip | canBreak | canPlace | infiniteItems |
//   |-----------|--------|--------|----------|----------|---------------|
//   | SPECTATOR |  Y     |  Y     |  N       |  N       |  Y            |
//   | CREATIVE  |  Y     |  Y     |  Y       |  Y       |  Y            |
//   | SURVIVAL  |  N     |  N     |  Y       |  Y       |  N            |
//   | ADVENTURE |  N     |  N     |  N       |  N       |  N            |
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "Common/Inventory.h"

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

static void test_spectator() {
    CHECK(GameModePerm::CanFly(GameMode::SPECTATOR), "spectator can fly");
    CHECK(GameModePerm::NoClip(GameMode::SPECTATOR), "spectator can noclip");
    CHECK(!GameModePerm::CanBreak(GameMode::SPECTATOR), "spectator cannot break");
    CHECK(!GameModePerm::CanPlace(GameMode::SPECTATOR), "spectator cannot place");
    CHECK(GameModePerm::InfiniteItems(GameMode::SPECTATOR), "spectator has infinite items");
}

static void test_creative() {
    CHECK(GameModePerm::CanFly(GameMode::CREATIVE), "creative can fly");
    CHECK(GameModePerm::NoClip(GameMode::CREATIVE), "creative can noclip (dev behavior)");
    CHECK(GameModePerm::CanBreak(GameMode::CREATIVE), "creative can break");
    CHECK(GameModePerm::CanPlace(GameMode::CREATIVE), "creative can place");
    CHECK(GameModePerm::InfiniteItems(GameMode::CREATIVE), "creative has infinite items");
}

static void test_survival() {
    CHECK(!GameModePerm::CanFly(GameMode::SURVIVAL), "survival cannot fly");
    CHECK(!GameModePerm::NoClip(GameMode::SURVIVAL), "survival cannot noclip");
    CHECK(GameModePerm::CanBreak(GameMode::SURVIVAL), "survival can break");
    CHECK(GameModePerm::CanPlace(GameMode::SURVIVAL), "survival can place");
    CHECK(!GameModePerm::InfiniteItems(GameMode::SURVIVAL), "survival has no infinite items");
}

static void test_adventure() {
    CHECK(!GameModePerm::CanFly(GameMode::ADVENTURE), "adventure cannot fly");
    CHECK(!GameModePerm::NoClip(GameMode::ADVENTURE), "adventure cannot noclip");
    CHECK(!GameModePerm::CanBreak(GameMode::ADVENTURE), "adventure cannot break");
    CHECK(!GameModePerm::CanPlace(GameMode::ADVENTURE), "adventure cannot place");
    CHECK(!GameModePerm::InfiniteItems(GameMode::ADVENTURE), "adventure has no infinite items");
}

static void test_names() {
    CHECK(std::strcmp(GameModeName(GameMode::SURVIVAL), "SURVIVAL") == 0, "survival name");
    CHECK(std::strcmp(GameModeName(GameMode::CREATIVE), "CREATIVE") == 0, "creative name");
    CHECK(std::strcmp(GameModeName(GameMode::ADVENTURE), "ADVENTURE") == 0, "adventure name");
    CHECK(std::strcmp(GameModeName(GameMode::SPECTATOR), "SPECTATOR") == 0, "spectator name");
    CHECK(std::strcmp(GameModeName(static_cast<GameMode>(99)), "UNKNOWN") == 0, "unknown name");
}

int main() {
    test_spectator();
    test_creative();
    test_survival();
    test_adventure();
    test_names();
    printf("%d/%d gamemode permission checks passed\n", g_passed, g_tests);
    return g_failed ? 1 : 0;
}
