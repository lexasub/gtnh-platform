// Standalone TOCTOU test binary — no echo test dependency.
// The echo test has a pre-existing segfault so we run TOCTOU tests in isolation.
#include "test.h"

int g_tests = 0, g_passed = 0, g_failed = 0;

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

#include "test_toctou.cpp"

int main() {
    printf("=== TOCTOU standalone test ===\n\n");

    ++g_tests; printf("  TEST: pack_overflow\n");
    test_frame_pack_overflow_produces_zeros();

    ++g_tests; printf("  TEST: stale_read_corruption\n");
    test_stale_read_corruption();

    ++g_tests; printf("  TEST: single_ring_toctou\n");
    test_single_ring_toctou();

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
