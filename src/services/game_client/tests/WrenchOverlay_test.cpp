// Wrench overlay hit-test tests. The geometry (cube-corner indexing, face
// order, bar click boxes) is the spec drawn by ImGuiOverlay — these assert the
// extracted HitTestWrenchBar returns the expected face for known screen points.
#include <cstdio>
#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Render/WrenchOverlay.h"

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

// Orthographic setup: camera at the origin looking down -Z, block 5 units in
// front. With a tight frustum (-1..1) the side bars land at opposite screen
// edges (see WrenchOverlay.cpp corner/face layout): +X bar midpoint (100, 25),
// -X bar midpoint (50, 25) — far enough apart that the 14px click tolerance
// does not overlap them.
static glm::mat4 test_view() { return glm::mat4(1.0f); }
static glm::mat4 test_proj() { return glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 100.0f); }

static void test_hit_plus_x_bar() {
    const int w = 100, h = 100;
    const glm::vec3 cam(0, 0, 0);
    const BlockPos hb{0, 0, -5};
    int r = wrench_overlay::HitTestWrenchBar(test_view(), test_proj(), w, h, cam, hb, 100.0, 25.0);
    CHECK_EQ(r, 0, "mouse on +X side bar -> face 0");
}

static void test_hit_minus_x_bar() {
    const int w = 100, h = 100;
    const glm::vec3 cam(0, 0, 0);
    const BlockPos hb{0, 0, -5};
    int r = wrench_overlay::HitTestWrenchBar(test_view(), test_proj(), w, h, cam, hb, 50.0, 25.0);
    CHECK_EQ(r, 1, "mouse on -X side bar -> face 1");
}

static void test_miss_returns_minus_one() {
    const int w = 100, h = 100;
    const glm::vec3 cam(0, 0, 0);
    const BlockPos hb{0, 0, -5};
    int r = wrench_overlay::HitTestWrenchBar(test_view(), test_proj(), w, h, cam, hb, 75.0, 25.0);
    CHECK_EQ(r, -1, "mouse away from bars -> -1");
}

static void test_deterministic() {
    const int w = 100, h = 100;
    const glm::vec3 cam(0, 0, 0);
    const BlockPos hb{0, 0, -5};
    int a = wrench_overlay::HitTestWrenchBar(test_view(), test_proj(), w, h, cam, hb, 100.0, 25.0);
    int b = wrench_overlay::HitTestWrenchBar(test_view(), test_proj(), w, h, cam, hb, 100.0, 25.0);
    CHECK_EQ(a, b, "repeated call -> same result");
}

int main() {
    test_hit_plus_x_bar();
    test_hit_minus_x_bar();
    test_miss_returns_minus_one();
    test_deterministic();
    printf("%d/%d wrench overlay hit-test checks passed\n", g_passed, g_tests);
    return g_failed ? 1 : 0;
}
