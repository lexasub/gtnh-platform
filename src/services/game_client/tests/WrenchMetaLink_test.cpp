// Wrench pipe-meta link tests.
//
// Verifies the END-TO-END agreement between the SERVER meta layout
// (simulation_core WrenchMeta.h: bit i connects face i, index order
// {+X,-X,+Y,-Y,+Z,-Z}) and the CLIENT-side pipe mesh builder
// (PipeMeta.h / PipeMeshBuilder.cpp): a server-meta bit set for face i must
// make the client draw the connection on exactly that world face, and a
// cleared bit must remove it. Regression guard for the class of bugs where
// the client interprets meta bits with a permuted face order, so clicking a
// face toggles a connection on a DIFFERENT (invisible) face and the user
// sees "wrench does nothing".
#include <cstdio>
#include <cstdint>
#include <functional>

#include "Render/PipeMeta.h"
#include "Render/PipeMeshBuilder.h"
#include "simulation_core/Actions/handTool/WrenchMeta.h"

static int g_tests = 0, g_passed = 0, g_failed = 0;

static void test_check(bool cond, const char* file, int line, const char* expr,
                       const char* msg) {
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

// ---------------------------------------------------------------------------
// 1. Server layout, as proven by live logs: wireFace (0=DOWN,1=UP,2=NORTH,
//    3=SOUTH,4=WEST,5=EAST) toggles bit dir where dir order is {+X,-X,+Y,-Y,+Z,-Z}.
static void test_server_layout() {
    // Toggle each wireFace on a fully-connected (0x3F) host; the cleared bit
    // must be exactly one dir bit — a permutation of the 6 faces.
    const uint8_t canonical[6] = {1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5};
    for (int f = 0; f < 6; ++f) {
        auto r = simcore::computePipeToggle(static_cast<uint8_t>(f), 0x3F, 0x3F);
        uint8_t cleared = static_cast<uint8_t>(0x3F & ~r.hostMeta);
        // cleared must be exactly one bit, and that bit must be one of the 6
        // dir bits (a permutation).
        CHECK_EQ(cleared & (cleared - 1), 0, "exactly one bit cleared");
        CHECK(cleared != 0, "bit is not zero");
        bool found = false;
        for (int d = 0; d < 6; ++d) {
            if (cleared == canonical[d]) { found = true; break; }
        }
        CHECK(found, "cleared bit is a valid dir bit");
    }
    // Anchor cases from the live log session (server responses to clicks):
    //   face=5 (EAST/+X)  -> toggles bit0
    //   face=4 (WEST/-X)  -> toggles bit1
    //   face=1 (UP/+Y)    -> toggles bit2
    //   face=0 (DOWN/-Y)  -> toggles bit3
    //   face=3 (SOUTH/+Z) -> toggles bit4
    //   face=2 (NORTH/-Z) -> toggles bit5
    auto r5 = simcore::computePipeToggle(5, 0x3F, 0x3F);
    CHECK_EQ(r5.hostMeta, 0x3F & ~(1u << 0), "face 5 (EAST) toggles bit 0");
    auto r4 = simcore::computePipeToggle(4, 0x3F, 0x3F);
    CHECK_EQ(r4.hostMeta, 0x3F & ~(1u << 1), "face 4 (WEST) toggles bit 1");
    auto r1 = simcore::computePipeToggle(1, 0x3F, 0x3F);
    CHECK_EQ(r1.hostMeta, 0x3F & ~(1u << 2), "face 1 (UP) toggles bit 2");
    auto r0 = simcore::computePipeToggle(0, 0x3F, 0x3F);
    CHECK_EQ(r0.hostMeta, 0x3F & ~(1u << 3), "face 0 (DOWN) toggles bit 3");
    auto r3 = simcore::computePipeToggle(3, 0x3F, 0x3F);
    CHECK_EQ(r3.hostMeta, 0x3F & ~(1u << 4), "face 3 (SOUTH) toggles bit 4");
    auto r2 = simcore::computePipeToggle(2, 0x3F, 0x3F);
    CHECK_EQ(r2.hostMeta, 0x3F & ~(1u << 5), "face 2 (NORTH) toggles bit 5");
}

// ---------------------------------------------------------------------------
// 2. Client-side meta -> FaceMask conversion. Server bit dir (order
//    {+X,-X,+Y,-Y,+Z,-Z}) must map to the client geometry face:
//    +X->EAST, -X->WEST, +Y->UP, -Y->DOWN, +Z->SOUTH, -Z->NORTH.
static void test_client_conversion() {
    CHECK_EQ(metaToFaceMask(1u << 0), FACE_EAST,  "bit0 (+X) -> EAST");
    CHECK_EQ(metaToFaceMask(1u << 1), FACE_WEST,  "bit1 (-X) -> WEST");
    CHECK_EQ(metaToFaceMask(1u << 2), FACE_UP,    "bit2 (+Y) -> UP");
    CHECK_EQ(metaToFaceMask(1u << 3), FACE_DOWN,  "bit3 (-Y) -> DOWN");
    CHECK_EQ(metaToFaceMask(1u << 4), FACE_SOUTH, "bit4 (+Z) -> SOUTH");
    CHECK_EQ(metaToFaceMask(1u << 5), FACE_NORTH, "bit5 (-Z) -> NORTH");
    const FaceMask all = FACE_EAST | FACE_WEST | FACE_UP | FACE_DOWN |
                         FACE_SOUTH | FACE_NORTH;
    CHECK_EQ(metaToFaceMask(0x3F), all, "0x3F -> all six faces");
    CHECK_EQ(metaToFaceMask(0), 0, "meta 0 -> empty (callers normalize)");
}

// ---------------------------------------------------------------------------
// 3. End-to-end: detectConnections with a server-written meta byte.
static void test_detect_connections() {
    const uint16_t pipeId = pipeTypeToBlockId(PipeType::ITEM_PIPE);
    // Neighbour pipes on +X and -Z only.
    auto getBlock = [pipeId](int32_t x, int32_t y, int32_t z) -> uint16_t {
        if (x == 1 && y == 0 && z == 0) return pipeId;  // +X
        if (x == 0 && y == 0 && z == -1) return pipeId; // -Z
        if (x == 0 && y == 0 && z == 0) return pipeId;
        return 0;
    };
    PipeMeshBuilder b;

    // legacy meta 0 -> pure neighbour mask
    auto m0 = b.detectConnections(0, 0, 0, PipeType::ITEM_PIPE, getBlock,
                                  [](int32_t, int32_t, int32_t) { return 0; });
    CHECK_EQ(m0, FACE_EAST | FACE_NORTH, "meta 0 -> neighbour mask (legacy)");

    // meta has +X bit only -> EAST stays, NORTH is cut
    auto mX = b.detectConnections(0, 0, 0, PipeType::ITEM_PIPE, getBlock,
                                  [](int32_t, int32_t, int32_t) { return 1u << 0; });
    CHECK_EQ(mX, FACE_EAST, "meta bit0 (+X) -> EAST only");

    // meta has -Z bit only -> NORTH stays, EAST is cut
    auto mZ = b.detectConnections(0, 0, 0, PipeType::ITEM_PIPE, getBlock,
                                  [](int32_t, int32_t, int32_t) { return 1u << 5; });
    CHECK_EQ(mZ, FACE_NORTH, "meta bit5 (-Z) -> NORTH only");

    // meta has both -> both connections
    auto mXZ = b.detectConnections(0, 0, 0, PipeType::ITEM_PIPE, getBlock,
                                   [](int32_t, int32_t, int32_t) { return (1u << 0) | (1u << 5); });
    CHECK_EQ(mXZ, FACE_EAST | FACE_NORTH, "meta +X|-Z -> EAST|NORTH");

    // meta has NEITHER -> no connections at all
    auto mNone = b.detectConnections(0, 0, 0, PipeType::ITEM_PIPE, getBlock,
                                     [](int32_t, int32_t, int32_t) { return 1u << 3; });
    CHECK_EQ(mNone, 0, "meta -Y only -> no connections (neighbours not wired)");

    // meta 0x3F -> neighbour mask restored
    auto mAll = b.detectConnections(0, 0, 0, PipeType::ITEM_PIPE, getBlock,
                                    [](int32_t, int32_t, int32_t) { return 0x3F; });
    CHECK_EQ(mAll, FACE_EAST | FACE_NORTH, "meta 0x3F -> neighbour mask");
}

int main() {
    test_server_layout();
    test_client_conversion();
    test_detect_connections();
    printf("%d/%d passed\n", g_passed, g_tests);
    return g_failed == 0 ? 0 : 1;
}
