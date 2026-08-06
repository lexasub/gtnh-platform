// 7.3 ConsoleWindow arg-validation for /startGameScenario.
// Tests gamescenario::parseScenarioIndex + the display-only scenario table:
//   - empty arg  → invalid (no index filled)
//   - non-numeric → invalid
//   - out-of-range / unknown index → invalid (scenario 0 is the only one)
//   - valid "0" → fills index 0 and matches the scenario table
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string_view>

#include "UI/Windows/player/GameScenario.h"

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

static void test_parse_rejects_empty() {
    uint8_t idx = 99;
    CHECK_EQ(gamescenario::parseScenarioIndex("", idx), false, "empty arg rejected");
    CHECK_EQ(idx, uint8_t(99), "index unchanged on rejection");
    PASS();
}

static void test_parse_rejects_non_numeric() {
    uint8_t idx = 99;
    CHECK(!gamescenario::parseScenarioIndex("abc", idx), "non-numeric rejected");
    CHECK(!gamescenario::parseScenarioIndex("0x", idx), "trailing garbage rejected");
    CHECK(!gamescenario::parseScenarioIndex("-1", idx), "negative starts with '-' rejected");
    CHECK_EQ(idx, uint8_t(99), "index unchanged on rejection");
    PASS();
}

static void test_parse_rejects_unknown_or_overflow() {
    uint8_t idx = 99;
    // 256 overflows uint8 range.
    CHECK(!gamescenario::parseScenarioIndex("256", idx), "overflow rejected");
    // Valid uint8 but not a registered scenario (only 0 exists).
    CHECK(!gamescenario::parseScenarioIndex("3", idx), "unregistered index rejected");
    CHECK_EQ(idx, uint8_t(99), "no index change for invalid input");
    PASS();
}

static void test_parse_accepts_valid() {
    uint8_t idx = 0;
    CHECK(gamescenario::parseScenarioIndex("0", idx), "scenario 0 accepted");
    CHECK_EQ(idx, uint8_t(0), "index parsed to 0");
    bool found = false;
    for (const auto& sc : gamescenario::scenarios()) {
        if (sc.index == idx) found = true;
    }
    CHECK(found, "parsed index resolves in the client scenario table");
    PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

int main(int, char**) {
    printf("=== GameClient Scenario Command Validation ===\n\n");
    TEST(parse_rejects_empty);
    TEST(parse_rejects_non_numeric);
    TEST(parse_rejects_unknown_or_overflow);
    TEST(parse_accepts_valid);
    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}