#include <libgtnh-net/test/test.h>
#include "Actions/handTool/WrenchMeta.h"

#include <cstdint>
#include <cstdio>

extern int g_tests, g_passed, g_failed;
void test_check(bool cond, const char* file, int line, const char* expr, const char* msg);

#ifndef CHECK_EQ
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)
#endif

void test_wrench_meta() {
    using simcore::computePipeToggle;

    // All-connected host (meta 0 -> 0x3F). EAST (wire face 5 -> +X -> bit0).
    auto r = computePipeToggle(5, 0, 0);
    CHECK_EQ(r.hostMeta, uint8_t(0x3E), "EAST toggles host bit0 off (0x3F^1)");
    CHECK_EQ(r.neighborMeta, uint8_t(0x3D), "EAST toggles neighbor bit1 off (0x3F^2)");

    // Toggling the same face again restores all-connected.
    auto back = computePipeToggle(5, r.hostMeta, r.neighborMeta);
    CHECK_EQ(back.hostMeta, uint8_t(0x3F), "second EAST toggle restores host");
    CHECK_EQ(back.neighborMeta, uint8_t(0x3F), "second EAST toggle restores neighbor");

    // WEST (face 4 -> -X -> bit1) toggles the opposite bit.
    auto w = computePipeToggle(4, 0, 0);
    CHECK_EQ(w.hostMeta, uint8_t(0x3D), "WEST toggles host bit1 off");
    CHECK_EQ(w.neighborMeta, uint8_t(0x3E), "WEST toggles neighbor bit0 off");

    // UP (face 1 -> +Y -> bit2).
    auto u = computePipeToggle(1, 0, 0);
    CHECK_EQ(u.hostMeta, uint8_t(0x3B), "UP toggles host bit2 off (0x3F^4)");
    CHECK_EQ(u.neighborMeta, uint8_t(0x37), "UP toggles neighbor bit3 off (0x3F^8)");

    // DOWN (face 0 -> -Y -> bit3).
    auto d = computePipeToggle(0, 0, 0);
    CHECK_EQ(d.hostMeta, uint8_t(0x37), "DOWN toggles host bit3 off");
    CHECK_EQ(d.neighborMeta, uint8_t(0x3B), "DOWN toggles neighbor bit2 off");

    // NORTH (face 2 -> -Z -> bit5).
    auto n = computePipeToggle(2, 0, 0);
    CHECK_EQ(n.hostMeta, uint8_t(0x1F), "NORTH toggles host bit5 off (0x3F^0x20)");
    CHECK_EQ(n.neighborMeta, uint8_t(0x2F), "NORTH toggles neighbor bit4 off (0x3F^0x10)");

    // SOUTH (face 3 -> +Z -> bit4).
    auto s = computePipeToggle(3, 0, 0);
    CHECK_EQ(s.hostMeta, uint8_t(0x2F), "SOUTH toggles host bit4 off");
    CHECK_EQ(s.neighborMeta, uint8_t(0x1F), "SOUTH toggles neighbor bit5 off");

    // Host already partially disconnected (0x3E = EAST off): toggling EAST reconnects.
    auto rec = computePipeToggle(5, 0x3E, 0x3D);
    CHECK_EQ(rec.hostMeta, uint8_t(0x3F), "EAST toggle reconnects host");
    CHECK_EQ(rec.neighborMeta, uint8_t(0x3F), "EAST toggle reconnects neighbor");

    // Out-of-range face returns inputs unchanged.
    auto oob = computePipeToggle(6, 0x3E, 0x3D);
    CHECK_EQ(oob.hostMeta, uint8_t(0x3E), "face>5 leaves host unchanged");
    CHECK_EQ(oob.neighborMeta, uint8_t(0x3D), "face>5 leaves neighbor unchanged");
}
