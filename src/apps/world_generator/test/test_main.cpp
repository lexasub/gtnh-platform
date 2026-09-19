// world_generator integration tests — harness
#include <cstdint>
#include <cstdio>

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

#define CHECK(cond, msg)    test_check((cond), __FILE__, __LINE__, #cond, msg)
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)
#define CHECK_NE(a, b, msg) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, msg)
#define CHECK_GT(a, b, msg) test_check((a) > (b), __FILE__, __LINE__, #a " > " #b, msg)
#define PASS() do { ++g_passed; } while(0)
#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

// Forward declarations
void test_tree_generation();

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("=== world_generator test suite ===\n\n");

    test_tree_generation();

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
