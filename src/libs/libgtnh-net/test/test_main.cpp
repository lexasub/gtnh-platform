#include "test.h"
#include <cstdio>

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_frame();
void test_context();
void test_echo();
void test_toctou();

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

void test_check(bool cond, const char* file, int line, const char* expr, const char* msg) {
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    printf("=== libgtnh-net test suite ===\n\n");

    TEST(frame);
    TEST(context);
    TEST(echo);
    TEST(toctou);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
