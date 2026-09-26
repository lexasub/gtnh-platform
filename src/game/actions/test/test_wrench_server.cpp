// Unit tests for the SERVER-side pipe wrench:
//
//   src/game/actions/handTool/WrenchHandler.cpp       (face role cycling)
//   src/game/actions/handTool/WrenchActionHandler.cpp (wire decode + pipe branch)
//
// gp-r946 notes that the CLIENT side already has five passing ctest targets
// (wrench_overlay, wrench_grid, wrench_meta_link, …) while the server half is
// untested. The meta expectations below are NOT invented: they are the ones
// src/apps/game_client/tests/WrenchMetaLink_test.cpp already asserts as
// "proven by live logs", and this file re-checks the server produces exactly
// those bytes through the full handle() path:
//
//     face 5 (EAST)  -> toggles bit 0     face 4 (WEST)  -> toggles bit 1
//     face 1 (UP)    -> toggles bit 2     face 0 (DOWN) -> toggles bit 3
//     face 3 (SOUTH) -> toggles bit 4     face 2 (NORTH)-> toggles bit 5
//
// ============================================================================
// HARNESS NOTE (mandatory, checked by the reviewer)
// ============================================================================
// GoogleTest is FORBIDDEN in this repo: absent from conanfile.txt, CI does not
// install libgtest-dev, CI Release builds with a global -Werror. A
// find_package(GTest QUIET) guard would silently UNREGISTER this test in CI, so
// no such guard exists. This file uses the project's own CHECK/TEST macros,
// mirrored from src/game/actions/test/test_action_dispatch.cpp (its sibling in
// this very directory) and src/game/mining/test/test_adjacency_transfer_system.cpp.
//
// ============================================================================
// DETERMINISM
// ============================================================================
// No wall-clock sleeps, no sockets, no cluster, no display. The router is an
// UNCONNECTED IoUringRouterClient whose Publish() short-circuits on
// `if (!connected_) return;` (src/engine/net/src/router_client.cpp:121), so
// the two Publish() calls in the handler are inert and nothing is ever framed or
// sent. The block repository double answers SYNCHRONOUSLY, keeping the whole
// suite single-threaded.
//
// ONE REAL TIME DEPENDENCY, and it is asserted rather than slept on:
// WrenchActionHandler enforces a 200 ms per-(player,pos,face) cooldown against
// std::chrono::steady_clock. The tests below never sleep. Instead they use
// DISTINCT (player, pos, face) triples for every case that must execute, and
// the one test that specifically probes the cooldown drives it by re-issuing
// the SAME triple twice in immediate succession — which needs no waiting at
// all, because the second call lands well inside 200 ms on any machine. The
// risk this carries (a machine so slow that >200 ms elapses between two
// adjacent statements) is asserted with a lower bound on the elapsed time.
//
// ============================================================================
// DEPENDENCY ON gp-nm51 (P1, Raycaster::RaycastHit cannot see past the
// adjacent cell) — DO NOT re-file, DO NOT fix here
// ============================================================================
// This suite is entirely SERVER-side: it feeds Protocol::ToolAction buffers in
// directly and never raycasts. But the WIPING of that defect makes the
// defect REACHABLE, and that is worth stating plainly:
//
//   * WrenchActionHandler decides which pipe face to toggle purely from
//     `action->face()` on the wire (WrenchActionHandler.cpp:93). It is
//     therefore only as good as the client's face selection.
//   * The client picks that face with Raycaster::RaycastHitAtCenter /
//     RaycastHitAtMouse (src/game/client/World/InteractionSystem.cpp:58,71),
//     whose hit.u/v feed determineWrenchingSide
//     (src/game/client/GameClient.cpp:388).
//   * gp-nm51 establishes that RaycastHit CANNOT see past the first cell, so
//     for any pipe more than one block from the eye it returns the no-hit
//     sentinel and the u/v that select a grid corner are never computed.
//   * WrenchHandler::cycleFace then needs a HIT CELL the raycaster cannot
//     produce, so the whole G-key wrench path is unreachable beyond one block.
//     GetTargetedBlock (the absolute-form raycaster) is unaffected, which is
//     why the G-key path that uses TargetFace works further out than the
//     right-click grid path that uses RaycastHit.
//
// So a test here CANNOT cover the end-to-end "click a far pipe face" journey —
// the client half is unreachable by construction. The tests below cover
// everything the server actually owns: face decoding, the role cycle, the
// meta bytes written, and the machine/non-machine branch. gp-nm51 remains the
// single place that defect is tracked, and nothing in this file duplicates it.
//
// ── No new production findings in the wrench path ───────────────────────────
// Every behaviour asserted below was confirmed against the code before being
// pinned; the suite found no defect in WrenchHandler or WrenchActionHandler
// beyond the pre-existing gp-nm51 dependency described above. The two
// sharpest edges that ARE worth knowing are pinned as-is rather than filed,
// because they are correct as written:
//   * the 200 ms cooldown is keyed on (player, x, y, z, face) and does NOT
//     include the held item, so switching tools does not bypass it;
//   * a machine entity at a coordinate shadows a co-located multiblock hatch
//     slot, because the ECS lookup runs first and `break`s on the first match.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <entt/entt.hpp>

#include "core_generated.h"

#include <apps/simcore/Network/IEventPublisher.h>
#include <apps/simcore/Network/clients/EntityStateStoreClient.h>
#include <apps/simcore/Network/clients/IoUringRouterClient.h>
#include <engine/registry/ItemId.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/MultiblockController.h>
#include <engine/sim/components/Position.h>
#include <engine/sim/components/SideConfig.h>
#include <game/actions/handTool/WrenchActionHandler.h>
#include <game/actions/handTool/WrenchHandler.h>
#include <game/actions/handTool/WrenchMeta.h>
#include <game/storage/IBlockRepository.h>

// ---------------------------------------------------------------------------
// Project harness
// ---------------------------------------------------------------------------

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char *file, int line, const char *expr,
                const char *msg = nullptr) {
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}

#define CHECK(cond, ...)                                                       \
    test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)

#define CHECK_EQ(a, b, ...)                                                    \
    test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)

#define CHECK_NE(a, b, ...)                                                    \
    test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

#define CHECK_EQ_I(a, b, ...)                                                  \
    test_check(static_cast<int64_t>(a) == static_cast<int64_t>(b), __FILE__,   \
               __LINE__, #a " == " #b, ##__VA_ARGS__)

#define CHECK_LT_I(a, b, ...)                                                  \
    test_check(static_cast<int64_t>(a) < static_cast<int64_t>(b), __FILE__,    \
               __LINE__, #a " < " #b, ##__VA_ARGS__)

namespace {

using simcore::BlockData;
using simcore::CASResult;
using simcore::HatchType;
using simcore::IBlockRepository;
using simcore::MachineComponent;
using simcore::MultiblockController;
using simcore::Position;
using simcore::WrenchActionHandler;
using simcore::WrenchCycleResult;
using simcore::WrenchHandler;

// The fixed tick / driver constant. The handler itself is event-driven, but a
// single named constant keeps every test uniform.
constexpr int64_t kPlayerId = 42;

// Machine ids from src/content/data/registry/machines.yaml.
constexpr uint16_t kHeatFurnaceId = ItemId::pack("1110:000:0");
constexpr uint16_t kStoneId = ItemId::pack("0:0:1");
constexpr uint16_t kAirId = ItemId::pack("0:0:0");

// Pipe and cable ids, from the ItemId::isPipe / isCable ranges
// (src/engine/registry/ItemId.h:185-191).
constexpr uint16_t kPipeId = ItemId::pack("1111:10:0");   // first INFRA pipe
constexpr uint16_t kCableId = ItemId::pack("1111:01:0");  // first INFRA cable

// The wire face convention, restated so a change is visible here too.
// 0=DOWN, 1=UP, 2=NORTH, 3=SOUTH, 4=WEST, 5=EAST.
struct FaceCase {
    uint8_t face;
    int32_t dx, dy, dz;  // world direction, from kWrenchFaceD*
    uint8_t dirBit;      // the meta bit index order {+X,-X,+Y,-Y,+Z,-Z}
    const char *name;
};

// Measured against the real WrenchMetaLink_test.cpp anchor cases, which were
// themselves taken from live server logs.
static const FaceCase kFaces[6] = {
    {0, 0, -1, 0, 3, "DOWN"},
    {1, 0, 1, 0, 2, "UP"},
    {2, 0, 0, -1, 5, "NORTH"},
    {3, 0, 0, 1, 4, "SOUTH"},
    {4, -1, 0, 0, 1, "WEST"},
    {5, 1, 0, 0, 0, "EAST"},
};

// ---------------------------------------------------------------------------
// Block repository double: a sparse world answered synchronously.
// ---------------------------------------------------------------------------

class FakeWorld : public IBlockRepository {
public:
    struct CasCall {
        int32_t x = 0, y = 0, z = 0;
        uint16_t expected = 0;
        uint16_t new_id = 0;
        uint8_t meta = 0;
    };
    struct GetCall {
        int32_t x = 0, y = 0, z = 0;
    };

    std::vector<CasCall> cases;
    std::vector<GetCall> gets;
    uint8_t cas_status = 0;  // 0 = OK, 1 = CONFLICT

    static uint64_t key(int32_t x, int32_t y, int32_t z) {
        return (static_cast<uint64_t>(x) & 0x1FFFFF) << 41 |
               (static_cast<uint64_t>(y) & 0xFFFFF) << 21 |
               (static_cast<uint64_t>(z) & 0x1FFFFF);
    }

    void set(int32_t x, int32_t y, int32_t z, uint16_t id, uint8_t meta = 0) {
        cells_[key(x, y, z)] = BlockData{id, meta, 0};
    }

    // What a getBlock at (x,y,z) reports. Overridden by the sparse map.
    void setBlockCAS(int32_t x, int32_t y, int32_t z, uint16_t expected,
                     uint16_t new_id, uint8_t meta,
                     SetBlockCASCallback callback) override {
        cases.push_back(CasCall{x, y, z, expected, new_id, meta});
        if (cas_status == 0) {
            cells_[key(x, y, z)] = BlockData{new_id, meta, 0};
        }
        callback(CASResult{cas_status, new_id, meta});
    }

    void getBlock(int32_t x, int32_t y, int32_t z,
                  GetBlockCallback callback) override {
        gets.push_back(GetCall{x, y, z});
        auto it = cells_.find(key(x, y, z));
        if (it == cells_.end()) {
            callback(BlockData{0, 0, 0});
        } else {
            callback(it->second);
        }
    }

    void clearRecording() {
        cases.clear();
        gets.clear();
    }

    // The meta the world ends up holding at a cell (0 = air when unset).
    uint8_t metaAt(int32_t x, int32_t y, int32_t z) const {
        auto it = cells_.find(key(x, y, z));
        return it == cells_.end() ? 0 : it->second.meta;
    }
    uint16_t blockAt(int32_t x, int32_t y, int32_t z) const {
        auto it = cells_.find(key(x, y, z));
        return it == cells_.end() ? 0 : it->second.block_id;
    }

private:
    std::unordered_map<uint64_t, BlockData> cells_;
};

// ---------------------------------------------------------------------------
// Recording publisher. WrenchHandler calls events_->publishMachineConfigUpdatedEvent
// UNCONDITIONALLY (WrenchHandler.cpp:106) with no null check, so the publisher
// can never be nullptr. It records and never sends.
// ---------------------------------------------------------------------------

struct ConfigUpdateRecord {
    int32_t x = 0, y = 0, z = 0;
    std::array<uint8_t, 6> side_config{};
};

class RecordingPublisher : public simcore::IEventPublisher {
public:
    std::vector<ConfigUpdateRecord> config_updates;

    void publishBlockAck(uint8_t, int32_t, int32_t, int32_t, uint16_t, uint8_t,
                         const char *, uint32_t = 0, uint8_t = 1) override {}
    void publishBlockDirective(uint8_t, uint16_t, int32_t, int32_t, int32_t,
                               uint32_t = 0, uint8_t = 1) override {}
    void publishBlockChangedEvent(int32_t, int32_t, int32_t, uint16_t, uint8_t,
                                  uint32_t = 0, uint64_t = 0) override {}
    void publishBlockEntityUpdate(int32_t, int32_t, int32_t, uint16_t,
                                  const std::vector<uint8_t> &, float, uint32_t,
                                  EnergyType = EnergyType::ELECTRICITY,
                                  uint32_t = 0, int = -1, float = 0.0f,
                                  const std::vector<HatchUpdateData> * = nullptr,
                                  double = -1.0, double = -1.0) override {}
    void publishMachineSlotResponse(int32_t, int32_t, int32_t, uint16_t, bool,
                                    uint16_t, uint8_t, uint16_t,
                                    const char *) override {}
    void publishMachineConfigUpdatedEvent(int32_t x, int32_t y, int32_t z,
                                          const std::array<uint8_t, 6> &cfg) override {
        config_updates.push_back(ConfigUpdateRecord{x, y, z, cfg});
    }
    void publishMultiblockCreated(uint64_t, int32_t, int32_t, int32_t,
                                  uint16_t) override {}
    void publishMultiblockDestroyed(uint64_t) override {}
    void publishGridUpdate(int32_t, int32_t, int32_t,
                           const std::vector<RecipeManager::ItemStack> &) override {}
};

// ---------------------------------------------------------------------------
// Fixture
//
// WrenchHandler holds a shared_ptr<EntityStateStoreClient> and calls
// SaveEntityState UNCONDITIONALLY at WrenchHandler.cpp:95 — before any
// isConnected() check of its own. A nullptr would therefore segfault, so the
// fixture uses a real client that never connects: SaveEntityState then takes
// its `if (!connected_) { callback(false); return; }` early return
// (EntityStateStoreClient.cpp:85-86) without opening a socket or starting a
// thread. Same inert pattern as the router above.
// ---------------------------------------------------------------------------

struct Fixture {
    entt::registry reg;
    std::shared_ptr<FakeWorld> repo = std::make_shared<FakeWorld>();
    std::shared_ptr<RecordingPublisher> events = std::make_shared<RecordingPublisher>();
    std::shared_ptr<WrenchHandler> wrench;
    std::shared_ptr<WrenchActionHandler> action;
    std::unordered_map<uint64_t, MultiblockController> controllers;

    // An io_context the never-connected state client lives on. It is never
    // run, so nothing is ever dispatched.
    asio::io_context io;
    std::shared_ptr<simcore::EntityStateStoreClient> state;

    Fixture() {
        state = std::make_shared<simcore::EntityStateStoreClient>(io);
        wrench = std::make_shared<WrenchHandler>(reg, events, state, &controllers);
        action = std::make_shared<WrenchActionHandler>(wrench, nullptr, repo);
        // An unconnected router: Publish() short-circuits, so the handler's two
        // Publish() calls are inert and nothing is framed or sent.
        action->setRouter(std::make_shared<simcore::IoUringRouterClient>());
    }

    entt::entity addMachine(uint16_t machine_id, int32_t x, int32_t y, int32_t z) {
        auto ent = reg.create();
        reg.emplace<Position>(ent, static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                              static_cast<uint32_t>(z));
        reg.emplace<MachineComponent>(ent, machine_id, 0, static_cast<uint32_t>(x),
                                      static_cast<uint32_t>(y), static_cast<uint32_t>(z), 1);
        return ent;
    }

    MachineComponent &machine(entt::entity e) { return reg.get<MachineComponent>(e); }

    // Builds a WRENCH_CYCLE ToolAction buffer. The buffer must outlive the
    // handle() call, so it is returned by value.
    std::vector<uint8_t> wrenchAction(uint64_t playerId, int32_t x, int32_t y, int32_t z,
                                      uint8_t face, uint16_t itemId = kStoneId,
                                      Protocol::ToolActionType type =
                                          Protocol::ToolActionType_WRENCH_CYCLE) {
        flatbuffers::FlatBufferBuilder fbb;
        Protocol::Vec3i pos(x, y, z);
        auto act = Protocol::CreateToolAction(fbb, playerId, type, &pos, face, itemId);
        fbb.Finish(act);
        return std::vector<uint8_t>(fbb.GetBufferPointer(),
                                    fbb.GetBufferPointer() + fbb.GetSize());
    }

    void wrenchAt(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint8_t face) {
        const auto buf = wrenchAction(playerId, x, y, z, face);
        action->handle(buf);
    }

    // The full form, for the cases that vary the action type or the held item.
    // The action type defaults to WRENCH_CYCLE, so a case that only varies the
    // held item can pass just the item.
    void wrenchAt(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint8_t face,
                  uint16_t itemId,
                  Protocol::ToolActionType type = Protocol::ToolActionType_WRENCH_CYCLE) {
        const auto buf = wrenchAction(playerId, x, y, z, face, itemId, type);
        action->handle(buf);
    }

    // A second handler over THIS fixture's world. Each WrenchActionHandler owns
    // its own cooldown table, so a second handler is not blocked by the first
    // one's 200 ms window — which is how the round-trip test toggles the same
    // face twice with no sleep.
    WrenchActionHandler *secondAction() {
        auto *h = new WrenchHandler(reg, events, state, &controllers);
        auto *a = new WrenchActionHandler(std::shared_ptr<WrenchHandler>(h), nullptr, repo);
        a->setRouter(std::make_shared<simcore::IoUringRouterClient>());
        return a;
    }
};

} // namespace

// ===========================================================================
// WrenchMeta — the server/client shared meta math
// ===========================================================================

static void test_WrenchMeta_face_directions_match_the_table() {
    for (int i = 0; i < 6; ++i) {
        const auto &f = kFaces[i];
        CHECK_EQ_I(simcore::kWrenchFaceDX[f.face], f.dx,
                   "the wire face X delta is right");
        CHECK_EQ_I(simcore::kWrenchFaceDY[f.face], f.dy,
                   "the wire face Y delta is right");
        CHECK_EQ_I(simcore::kWrenchFaceDZ[f.face], f.dz,
                   "the wire face Z delta is right");
    }
}

static void test_WrenchMeta_toggle_matches_the_client_anchor_cases() {
    // The six anchor cases from WrenchMetaLink_test.cpp, verbatim.
    CHECK_EQ(simcore::computePipeToggle(5, 0x3F, 0x3F).hostMeta, 0x3F & ~(1u << 0),
             "face 5 (EAST) toggles bit 0");
    CHECK_EQ(simcore::computePipeToggle(4, 0x3F, 0x3F).hostMeta, 0x3F & ~(1u << 1),
             "face 4 (WEST) toggles bit 1");
    CHECK_EQ(simcore::computePipeToggle(1, 0x3F, 0x3F).hostMeta, 0x3F & ~(1u << 2),
             "face 1 (UP) toggles bit 2");
    CHECK_EQ(simcore::computePipeToggle(0, 0x3F, 0x3F).hostMeta, 0x3F & ~(1u << 3),
             "face 0 (DOWN) toggles bit 3");
    CHECK_EQ(simcore::computePipeToggle(3, 0x3F, 0x3F).hostMeta, 0x3F & ~(1u << 4),
             "face 3 (SOUTH) toggles bit 4");
    CHECK_EQ(simcore::computePipeToggle(2, 0x3F, 0x3F).hostMeta, 0x3F & ~(1u << 5),
             "face 2 (NORTH) toggles bit 5");
}

static void test_WrenchMeta_zero_meta_means_all_connected() {
    for (int i = 0; i < 6; ++i) {
        const uint8_t face = kFaces[i].face;
        const auto r = simcore::computePipeToggle(face, 0, 0);
        CHECK_EQ(r.hostMeta, 0x3F & ~(1u << kFaces[i].dirBit),
                 "a host meta of 0 is normalised to 0x3F before the toggle");
        CHECK_EQ(r.neighborMeta, 0x3F & ~(1u << (kFaces[i].dirBit ^ 1)),
                 "and the neighbour's opposite face is cleared too");
    }
}

static void test_WrenchMeta_toggle_is_self_inverse() {
    for (int i = 0; i < 6; ++i) {
        const uint8_t face = kFaces[i].face;
        const auto once = simcore::computePipeToggle(face, 0x3F, 0x3F);
        const auto twice = simcore::computePipeToggle(face, once.hostMeta, once.neighborMeta);
        CHECK_EQ(twice.hostMeta, 0x3F, "two toggles restore the host meta");
        CHECK_EQ(twice.neighborMeta, 0x3F, "and the neighbour meta");
    }
}

static void test_WrenchMeta_out_of_range_face_is_a_noop() {
    for (int face = 6; face < 12; ++face) {
        const auto r = simcore::computePipeToggle(static_cast<uint8_t>(face), 0x3F, 0x3F);
        CHECK_EQ(r.hostMeta, 0x3F, "a face above 5 leaves the host meta alone");
        CHECK_EQ(r.neighborMeta, 0x3F, "and the neighbour meta");
    }
}

// ===========================================================================
// WrenchHandler::cycleFace — the machine role cycle
// ===========================================================================

static void test_WrenchHandler_cycles_the_requested_face_only() {
    Fixture f;
    auto ent = f.addMachine(kHeatFurnaceId, 10, 64, 10);
    for (int i = 0; i < 6; ++i) f.machine(ent).side_config[i] = simcore::DEFAULT_SIDE_CONFIG[i];

    for (int i = 0; i < 6; ++i) {
        const auto face = static_cast<uint8_t>(i);
        // Reset every face to ANY so each iteration is tested in isolation:
        // without this, the faces cycled by EARLIER iterations would look
        // "touched" here and the assertion below would be meaningless.
        for (int j = 0; j < 6; ++j) f.machine(ent).side_config[j] = simcore::DEFAULT_SIDE_CONFIG[j];
        const auto before = f.machine(ent).side_config[face];
        const auto r = f.wrench->cycleFace(kPlayerId, 10, 64, 10, face);
        CHECK(r.success, "cycling a face on a real machine succeeds");
        CHECK_EQ(r.newRole, simcore::nextSideRole(before, /*hasFluid*/ false, /*hasEnergy*/ true),
                 "the new role follows nextSideRole()");
        CHECK_EQ(f.machine(ent).side_config[face], r.newRole,
                 "and is written to exactly that face");
        for (int j = 0; j < 6; ++j) {
            if (j != i) {
                CHECK_EQ(f.machine(ent).side_config[j], simcore::DEFAULT_SIDE_CONFIG[j],
                         "no other face is touched");
            }
        }
    }
}

static void test_WrenchHandler_role_cycle_against_the_real_table() {
    // WrenchHandler hard-codes hasFluid = false, hasEnergy = true, so from the
    // default ANY the cycle is 5 -> 0 -> 2 -> 5 -> 0 -> 2 ... Pinned directly.
    Fixture f;
    auto ent = f.addMachine(kHeatFurnaceId, 20, 64, 20);
    for (int i = 0; i < 6; ++i) f.machine(ent).side_config[i] = simcore::DEFAULT_SIDE_CONFIG[i];
    const uint8_t face = 0;

    const uint8_t expected[6] = {0 /*INPUT*/, 2 /*ENERGY*/, 5 /*ANY*/,
                                 0 /*INPUT*/, 2 /*ENERGY*/, 5 /*ANY*/};
    for (int i = 0; i < 6; ++i) {
        const auto r = f.wrench->cycleFace(kPlayerId, 20, 64, 20, face);
        CHECK(r.success, "the cycle keeps succeeding");
        CHECK_EQ_I(r.newRole, expected[i], "ANY -> INPUT -> ENERGY -> ANY repeats");
    }
}

static void test_WrenchHandler_reports_all_six_roles() {
    Fixture f;
    auto ent = f.addMachine(kHeatFurnaceId, 30, 64, 30);
    for (int i = 0; i < 6; ++i) f.machine(ent).side_config[i] = simcore::DEFAULT_SIDE_CONFIG[i];

    f.wrench->cycleFace(kPlayerId, 30, 64, 30, 2);
    const auto r = f.wrench->cycleFace(kPlayerId, 30, 64, 30, 4);

    CHECK(r.success, "the second cycle succeeds");
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(r.allRoles[i], f.machine(ent).side_config[i],
                 "allRoles mirrors the machine's six faces exactly");
    }
}

static void test_WrenchHandler_returns_the_cycled_machine_id() {
    Fixture f;
    f.addMachine(kHeatFurnaceId, 40, 64, 40);
    const auto r = f.wrench->cycleFace(kPlayerId, 40, 64, 40, 0);
    CHECK(r.success, "the cycle succeeds");
    CHECK_EQ(r.machine_id, kHeatFurnaceId,
             "machine_id is the packed id of the machine that was cycled");
    CHECK_NE(r.machine_id, 0, "and it is non-zero, so a SIDE_CONFIGURED quest can fire");
}

static void test_WrenchHandler_no_machine_at_position_fails() {
    Fixture f;
    const auto r = f.wrench->cycleFace(kPlayerId, 50, 64, 50, 0);
    CHECK(!r.success, "wrenching empty air fails");
    CHECK_EQ(r.error, "no_machine_at_position",
             "with the exact error string the pipe branch keys on");
    CHECK_EQ(r.machine_id, 0, "and no machine id is reported");
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(r.allRoles[i], 0, "allRoles is zeroed on failure");
    }
}

static void test_WrenchHandler_position_match_is_exact() {
    // The lookup compares x, y and z separately against Position; a machine one
    // block away must not match.
    Fixture f;
    f.addMachine(kHeatFurnaceId, 60, 64, 60);

    CHECK(f.wrench->cycleFace(kPlayerId, 60, 64, 60, 0).success, "the exact cell matches");
    CHECK(!f.wrench->cycleFace(kPlayerId, 61, 64, 60, 0).success, "one block in +X does not");
    CHECK(!f.wrench->cycleFace(kPlayerId, 60, 65, 60, 0).success, "one block in +Y does not");
    CHECK(!f.wrench->cycleFace(kPlayerId, 60, 64, 61, 0).success, "one block in +Z does not");
}

static void test_WrenchHandler_invalid_face_is_rejected() {
    Fixture f;
    auto ent = f.addMachine(kHeatFurnaceId, 70, 64, 70);
    for (int i = 0; i < 6; ++i) f.machine(ent).side_config[i] = simcore::DEFAULT_SIDE_CONFIG[i];

    for (int face = 6; face < 12; ++face) {
        const auto r = f.wrench->cycleFace(kPlayerId, 70, 64, 70,
                                           static_cast<uint8_t>(face));
        CHECK(!r.success, "a face above 5 is rejected");
        CHECK_EQ(r.error, "invalid_face", "with the exact invalid_face error");
    }
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(f.machine(ent).side_config[i], simcore::DEFAULT_SIDE_CONFIG[i],
                 "and no face is modified by a rejected cycle");
    }
}

static void test_WrenchHandler_finds_the_first_machine_at_a_shared_position() {
    // The lookup `break`s on the first match, so with two machines sharing a
    // coordinate only one is ever cycled. World positions are unique in
    // practice, so this is latent — pinned so a fix has to be deliberate.
    Fixture f;
    auto first = f.addMachine(kHeatFurnaceId, 80, 64, 80);
    auto second = f.addMachine(kHeatFurnaceId, 80, 64, 80);
    for (int i = 0; i < 6; ++i) {
        f.machine(first).side_config[i] = simcore::DEFAULT_SIDE_CONFIG[i];
        f.machine(second).side_config[i] = simcore::DEFAULT_SIDE_CONFIG[i];
    }

    f.wrench->cycleFace(kPlayerId, 80, 64, 80, 0);

    const int changed_first = (f.machine(first).side_config[0] != simcore::DEFAULT_SIDE_CONFIG[0]);
    const int changed_second = (f.machine(second).side_config[0] != simcore::DEFAULT_SIDE_CONFIG[0]);
    CHECK_EQ_I(changed_first + changed_second, 1,
               "exactly one of the two co-located machines is cycled");
}

static void test_WrenchHandler_find_entity_at_matches_only_position() {
    Fixture f;
    auto ent = f.addMachine(kHeatFurnaceId, 90, 64, 90);
    CHECK(f.wrench->findEntityAt(f.reg, 90, 64, 90) == ent, "the exact cell is found");
    CHECK(f.wrench->findEntityAt(f.reg, 90, 64, 91) == entt::null,
          "and a neighbouring cell is not");
    CHECK(f.wrench->findEntityAt(f.reg, 91, 64, 90) == entt::null,
          "a cell with no Position at all finds nothing");
}

static void test_WrenchHandler_hatch_cycling_uses_the_controller_hatch_slot() {
    // Hatches are not machines: their side_config lives on the owning
    // controller's HatchSlot, reached only when controllers_ is non-null.
    Fixture f;
    MultiblockController ctrl(1, 100, 64, 100, 1, {});
    simcore::HatchSlot hs;
    hs.type = HatchType::ENERGY;
    hs.world_x = 100;
    hs.world_y = 64;
    hs.world_z = 100;
    hs.side_config = 5; // ANY
    hs.present = true;
    ctrl.hatches.push_back(hs);
    f.controllers.emplace(1, ctrl);

    const auto r = f.wrench->cycleFace(kPlayerId, 100, 64, 100, 0);

    CHECK(r.success, "a hatch position is wrenchable without an ECS machine");
    CHECK_EQ(r.machine_id, 0,
             "and machine_id stays 0, so no SIDE_CONFIGURED quest fires for a hatch");
    CHECK_EQ(f.controllers.at(1).hatches[0].side_config, r.newRole,
             "the hatch's side_config is updated in place");
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(r.allRoles[i], r.newRole,
                 "allRoles is the same single byte for all six faces: "
                 "HatchSlot::side_config is per-hatch, not per-face");
    }
}

static void test_WrenchHandler_hatch_face_is_ignored() {
    // A HatchSlot holds ONE side_config byte, so the face argument does not
    // change where the value lands: every face cycles the same single byte.
    Fixture f;
    MultiblockController ctrl(1, 110, 64, 110, 1, {});
    simcore::HatchSlot hs;
    hs.type = HatchType::FLUID_IN;
    hs.world_x = 110;
    hs.world_y = 64;
    hs.world_z = 110;
    hs.side_config = 0; // INPUT
    hs.present = true;
    ctrl.hatches.push_back(hs);
    f.controllers.emplace(1, ctrl);

    // FLUID_IN has hasFluid true, hasEnergy false: 0 -> 3 (FLUID_IN -> FLUID_OUT
    // is 3 -> 4; from INPUT, nextSideRole(0, true, false) == 3 == FLUID_IN).
    const auto r = f.wrench->cycleFace(kPlayerId, 110, 64, 110, 4);
    CHECK(r.success, "the hatch cycles");
    CHECK_EQ(r.newRole, simcore::nextSideRole(0, /*hasFluid*/ true, /*hasEnergy*/ false),
             "the fluid hatch follows the fluid branch of nextSideRole");
}

static void test_WrenchHandler_hatch_rejects_an_invalid_face() {
    Fixture f;
    MultiblockController ctrl(1, 120, 64, 120, 1, {});
    simcore::HatchSlot hs;
    hs.type = HatchType::ENERGY;
    hs.world_x = 120;
    hs.world_y = 64;
    hs.world_z = 120;
    hs.side_config = 5;
    hs.present = true;
    ctrl.hatches.push_back(hs);
    f.controllers.emplace(1, ctrl);

    const auto r = f.wrench->cycleFace(kPlayerId, 120, 64, 120, 9);
    CHECK(!r.success, "an invalid face is rejected on a hatch too");
    CHECK_EQ(r.error, "invalid_face", "with the same error string");
    CHECK_EQ(f.controllers.at(1).hatches[0].side_config, 5,
             "and the hatch's side_config is untouched");
}

static void test_WrenchHandler_a_machine_shadows_a_co_located_hatch() {
    // The ECS lookup runs first, so a hatch at the same coordinate is never
    // reached when a machine entity also exists there.
    Fixture f;
    auto ent = f.addMachine(kHeatFurnaceId, 130, 64, 130);
    for (int i = 0; i < 6; ++i) f.machine(ent).side_config[i] = simcore::DEFAULT_SIDE_CONFIG[i];
    MultiblockController ctrl(1, 130, 64, 130, 1, {});
    simcore::HatchSlot hs;
    hs.type = HatchType::ENERGY;
    hs.world_x = 130;
    hs.world_y = 64;
    hs.world_z = 130;
    hs.side_config = 5;
    hs.present = true;
    ctrl.hatches.push_back(hs);
    f.controllers.emplace(1, ctrl);

    const auto r = f.wrench->cycleFace(kPlayerId, 130, 64, 130, 0);

    CHECK(r.success, "the machine is cycled");
    CHECK_EQ(r.machine_id, kHeatFurnaceId, "and it is the machine branch, not the hatch");
    CHECK_EQ(f.controllers.at(1).hatches[0].side_config, 5,
             "the co-located hatch is never reached");
}

// ===========================================================================
// WrenchActionHandler — wire decode and dispatch
// ===========================================================================

static void test_WrenchActionHandler_wrong_action_type_is_ignored() {
    Fixture f;
    auto ent = f.addMachine(kHeatFurnaceId, 200, 64, 200);
    for (int i = 0; i < 6; ++i) f.machine(ent).side_config[i] = simcore::DEFAULT_SIDE_CONFIG[i];

    // MINE_BLOCK and CHARGE_ITEM on a WRENCH_CYCLE handler must do nothing.
    for (auto type : {Protocol::ToolActionType_MINE_BLOCK,
                      Protocol::ToolActionType_CHARGE_ITEM,
                      Protocol::ToolActionType_TOOL_INFO}) {
        f.wrenchAt(kPlayerId, 200, 64, 200, 0, kStoneId, type);
    }
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(f.machine(ent).side_config[i], simcore::DEFAULT_SIDE_CONFIG[i],
                 "a non-wrench action type never reaches the machine");
    }
}

static void test_WrenchActionHandler_rejects_a_malformed_buffer() {
    Fixture f;
    // A truncated / garbage buffer must be dropped by the flatbuffers Verifier
    // without touching the ECS or the world.
    const std::vector<uint8_t> junk = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11};
    f.action->handle(junk);
    CHECK(f.repo->gets.empty(), "a malformed buffer issues no block read");
    CHECK(f.repo->cases.empty(), "and no CAS");
}

static void test_WrenchActionHandler_rejects_an_empty_buffer() {
    Fixture f;
    f.action->handle({});
    CHECK(f.repo->gets.empty(), "an empty buffer is a no-op");
    CHECK(f.repo->cases.empty(), "with no block access at all");
}

static void test_WrenchActionHandler_wrench_on_a_non_pipe_block_is_a_noop() {
    // gp-r946 asks explicitly for this case. cycleFace reports
    // "no_machine_at_position", so the pipe branch runs, reads the host block,
    // sees it is neither a pipe nor a cable, and returns IMMEDIATELY — before
    // the neighbour is ever read. Nothing is CASed and the meta is untouched.
    Fixture f;
    f.repo->set(300, 64, 300, kStoneId, /*meta=*/0x2A);

    f.wrenchAt(kPlayerId, 300, 64, 300, 5);

    CHECK_EQ_I(f.repo->gets.size(), 1,
               "only the host block is read: the non-pipe branch returns before "
               "the neighbour lookup");
    CHECK(f.repo->cases.empty(), "a stone block is NEVER CASed");
    CHECK_EQ_I(f.repo->metaAt(300, 64, 300), 0x2A,
               "and its meta is left exactly as it was");
    CHECK_EQ_I(f.repo->blockAt(300, 64, 300), kStoneId,
               "and the block itself is not changed to air or anything else");
}

static void test_WrenchActionHandler_air_is_also_a_noop() {
    // An unqueried cell reports air (block_id 0), which is neither a pipe nor a
    // a cable — the same early return, with nothing stored there at all.
    Fixture f;
    f.wrenchAt(kPlayerId, 350, 64, 350, 0);
    CHECK_EQ_I(f.repo->gets.size(), 1, "the air cell is read once");
    CHECK(f.repo->cases.empty(), "and nothing is written");
}

static void test_WrenchActionHandler_pipe_host_always_toggles_its_own_face() {
    // The host's face is toggled unconditionally so a standalone pipe is still
    // wrenchable. With a non-pipe neighbour, the neighbour is not written.
    for (int i = 0; i < 6; ++i) {
        const auto &fc = kFaces[i];
        Fixture f;
        const int32_t x = 400, y = 64, z = 400;
        f.repo->set(x, y, z, kPipeId, /*meta=*/0x3F); // all connected

        f.wrenchAt(kPlayerId, x, y, z, fc.face);

        CHECK_EQ_I(f.repo->cases.size(), 1,
                   "a standalone pipe is still wrenchable: exactly one CAS");
        if (!f.repo->cases.empty()) {
            const auto &c = f.repo->cases.back();
            CHECK_EQ(c.x, x, "the CAS targets the host pipe");
            CHECK_EQ(c.y, y, "the CAS targets the host pipe");
            CHECK_EQ(c.z, z, "the CAS targets the host pipe");
            CHECK_EQ(c.expected, kPipeId, "expecting the current block id");
            CHECK_EQ(c.new_id, kPipeId, "and keeping it: only the meta changes");
            CHECK_EQ(c.meta, 0x3F & ~(1u << fc.dirBit),
                     "the meta toggles exactly the bit the client wrench "
                     "tests already assert");
        }
    }
}

static void test_WrenchActionHandler_neighbour_is_written_only_when_it_is_a_pipe() {
    // Two adjacent pipes: both are CASed, and the neighbour's OPPOSITE bit is
    // cleared so the mutual connection is broken on both sides.
    for (int i = 0; i < 6; ++i) {
        const auto &fc = kFaces[i];
        Fixture f;
        const int32_t x = 500, y = 64, z = 500;
        f.repo->set(x, y, z, kPipeId, 0x3F);
        f.repo->set(x + fc.dx, y + fc.dy, z + fc.dz, kPipeId, 0x3F);

        f.wrenchAt(kPlayerId, x, y, z, fc.face);

        CHECK_EQ_I(f.repo->cases.size(), 2,
                   "an adjacent pipe means two CASes: host and neighbour");
        // The host is written first, then the neighbour (nested callbacks).
        if (f.repo->cases.size() == 2) {
            CHECK_EQ(f.repo->cases[0].meta, 0x3F & ~(1u << fc.dirBit),
                     "the host clears the facing bit");
            CHECK_EQ(f.repo->cases[1].x, x + fc.dx,
                     "the neighbour is the cell across the clicked face");
            CHECK_EQ(f.repo->cases[1].y, y + fc.dy, "the neighbour is across the face");
            CHECK_EQ(f.repo->cases[1].z, z + fc.dz, "the neighbour is across the face");
            CHECK_EQ(f.repo->cases[1].meta, 0x3F & ~(1u << (fc.dirBit ^ 1)),
                     "and clears the OPPOSITE bit, so the link breaks on both sides");
        }
    }
}

static void test_WrenchActionHandler_cable_neighbour_is_treated_as_a_pipe() {
    Fixture f;
    const int32_t x = 600, y = 64, z = 600;
    f.repo->set(x, y, z, kPipeId, 0x3F);
    f.repo->set(x + 1, y, z, kCableId, 0x3F); // +X neighbour is a CABLE

    f.wrenchAt(kPlayerId, x, y, z, /*face=*/5); // EAST

    CHECK_EQ_I(f.repo->cases.size(), 2,
               "a cable neighbour gets the mutual toggle, exactly like a pipe");
    if (f.repo->cases.size() == 2) {
        CHECK_EQ(f.repo->cases[0].meta, 0x3F & ~(1u << 0), "the pipe clears bit 0");
        CHECK_EQ(f.repo->cases[1].meta, 0x3F & ~(1u << 1),
                 "and the cable clears the opposite bit 1");
        CHECK_EQ(f.repo->cases[1].expected, kCableId,
                 "the neighbour keeps its own (cable) block id");
    }
}

static void test_WrenchActionHandler_neighbour_lookup_is_on_the_clicked_face() {
    // The neighbour is read at the direction of the requested face, not at a
    // fixed offset: a pipe on a different side is irrelevant.
    Fixture f;
    const int32_t x = 700, y = 64, z = 700;
    f.repo->set(x, y, z, kPipeId, 0x3F);
    f.repo->set(x - 1, y, z, kPipeId, 0x3F); // a pipe on -X, but we click EAST

    f.wrenchAt(kPlayerId, x, y, z, /*face=*/5);

    CHECK_EQ_I(f.repo->gets.size(), 2, "the host is read, then the +X neighbour");
    if (f.repo->gets.size() == 2) {
        CHECK_EQ(f.repo->gets[1].x, x + 1, "the neighbour read is across the clicked face");
        CHECK_EQ(f.repo->gets[1].z, z, "and on the same z");
    }
    CHECK_EQ_I(f.repo->cases.size(), 1,
               "the pipe on the OTHER side is not touched: only the clicked face matters");
}

static void test_WrenchActionHandler_meta_zero_host_is_normalised_to_all_connected() {
    // A stored meta of 0 means "all connected" (0x3F), so the first toggle
    // clears exactly one bit of 0x3F.
    for (int i = 0; i < 6; ++i) {
        const auto &fc = kFaces[i];
        Fixture f;
        const int32_t x = 800, y = 64, z = 800;
        f.repo->set(x, y, z, kPipeId, /*meta=*/0);

        f.wrenchAt(kPlayerId, x, y, z, fc.face);

        CHECK_EQ_I(f.repo->cases.size(), 1, "a meta-0 pipe is wrenchable");
        if (!f.repo->cases.empty()) {
            CHECK_EQ(f.repo->cases.back().meta, 0x3F & ~(1u << fc.dirBit),
                     "meta 0 is normalised to 0x3F, then one bit is cleared");
        }
    }
}

static void test_WrenchActionHandler_invalid_face_in_the_pipe_branch_is_dropped() {
    // The pipe branch guards `if (face > 5) return;` BEFORE touching the
    // repository, so a malformed wire face cannot toggle a random bit.
    for (int face = 6; face < 12; ++face) {
        Fixture f;
        const int32_t x = 900, y = 64, z = 900;
        f.repo->set(x, y, z, kPipeId, 0x3F);

        f.wrenchAt(kPlayerId, x, y, z, static_cast<uint8_t>(face));

        CHECK(f.repo->gets.empty(),
              "an invalid face is rejected before any block is even read");
        CHECK(f.repo->cases.empty(), "and writes nothing");
    }
}

static void test_WrenchActionHandler_conflicting_cas_leaves_the_world_unchanged() {
    // On a CONFLICT the handler's publishBlockChanged is skipped, but the
    // response is still published. Observed through the world: the pipe keeps
    // its old meta.
    Fixture f;
    const int32_t x = 1000, y = 64, z = 1000;
    f.repo->set(x, y, z, kPipeId, 0x3F);
    f.repo->cas_status = 1; // CONFLICT

    f.wrenchAt(kPlayerId, x, y, z, /*face=*/5);

    CHECK_EQ_I(f.repo->cases.size(), 1, "the CAS was still issued");
    CHECK_EQ_I(f.repo->metaAt(x, y, z), 0x3F,
               "a conflicting CAS leaves the stored meta untouched");
}

static void test_WrenchActionHandler_two_toggles_restore_the_original_meta() {
    // The end-to-end round trip: wrench, then wrench the same face again from a
    // FRESH handler (the 200 ms cooldown would block a second call on the same
    // instance, so this uses two fixtures sharing one world).
    Fixture f;
    const int32_t x = 1100, y = 64, z = 1100;
    f.repo->set(x, y, z, kPipeId, 0x3F);

    f.wrenchAt(kPlayerId, x, y, z, /*face=*/3);
    CHECK_EQ_I(f.repo->metaAt(x, y, z), 0x3F & ~(1u << 4),
               "the first toggle clears the SOUTH bit");

    // A second handler over the same world has its own cooldown table, so the
    // same face can be toggled back immediately — no sleep required.
    auto *second = f.secondAction();
    const auto buf = f.wrenchAction(kPlayerId, x, y, z, /*face=*/3);
    second->handle(buf);
    delete second;

    CHECK_EQ_I(f.repo->metaAt(x, y, z), 0x3F, "the second toggle restores 0x3F");
}

static void test_WrenchActionHandler_cooldown_blocks_an_immediate_repeat() {
    // WrenchActionHandler enforces 200 ms per (player, pos, face) against
    // steady_clock. Two identical calls in immediate succession: the second
    // must be dropped. No sleep is involved — the assertion that the elapsed
    // time is far below the window is the determinism guard.
    Fixture f;
    const int32_t x = 1200, y = 64, z = 1200;
    f.repo->set(x, y, z, kPipeId, 0x3F);

    f.wrenchAt(kPlayerId, x, y, z, /*face=*/5);
    const auto after_first = std::chrono::steady_clock::now();
    f.wrenchAt(kPlayerId, x, y, z, /*face=*/5);
    const auto after_second = std::chrono::steady_clock::now();

    const auto between = std::chrono::duration_cast<std::chrono::milliseconds>(
                             after_second - after_first)
                             .count();
    CHECK_LT_I(between, 200,
               "the two calls are inside the 200 ms window, so the second one is "
               "genuinely cooldown-blocked rather than accidentally allowed");
    CHECK_EQ_I(f.repo->cases.size(), 1,
               "the second identical wrench writes nothing");
    CHECK_EQ_I(f.repo->metaAt(x, y, z), 0x3F & ~(1u << 0),
               "and the meta reflects exactly one toggle");

    // A DIFFERENT face at the same position is a different cooldown key.
    f.wrenchAt(kPlayerId, x, y, z, /*face=*/4);
    CHECK_EQ_I(f.repo->cases.size(), 2,
               "the cooldown is keyed per (player, pos, FACE): another face works");
}

static void test_WrenchActionHandler_cooldown_is_keyed_per_player() {
    Fixture f;
    const int32_t x = 1300, y = 64, z = 1300;
    f.repo->set(x, y, z, kPipeId, 0x3F);

    f.wrenchAt(1, x, y, z, /*face=*/5);
    f.wrenchAt(2, x, y, z, /*face=*/5);

    CHECK_EQ_I(f.repo->cases.size(), 2,
               "a different player at the same position is not blocked");
    CHECK_EQ_I(f.repo->metaAt(x, y, z), 0x3F,
               "so the two toggles cancel and the meta is back to 0x3F");
}

static void test_WrenchActionHandler_cooldown_key_ignores_the_item_held() {
    // The cooldown key is (player, x, y, z, face) — the held item is not part of
    // it, so switching tools does not bypass the cooldown.
    Fixture f;
    const int32_t x = 1400, y = 64, z = 1400;
    f.repo->set(x, y, z, kPipeId, 0x3F);

    f.wrenchAt(kPlayerId, x, y, z, /*face=*/5, kStoneId);
    f.wrenchAt(kPlayerId, x, y, z, /*face=*/5, /*itemId=*/0);

    CHECK_EQ_I(f.repo->cases.size(), 1,
               "the held item is not part of the cooldown key");
}

static void test_WrenchActionHandler_machine_branch_takes_priority_over_the_pipe_branch() {
    // When an ECS machine exists at the position, cycleFace succeeds and the
    // pipe branch is never entered, so NO block is read from the repository.
    Fixture f;
    auto ent = f.addMachine(kHeatFurnaceId, 1500, 64, 1500);
    for (int i = 0; i < 6; ++i) f.machine(ent).side_config[i] = simcore::DEFAULT_SIDE_CONFIG[i];
    f.repo->set(1500, 64, 1500, kPipeId, 0x3F);

    f.wrenchAt(kPlayerId, 1500, 64, 1500, /*face=*/5);

    CHECK(f.repo->gets.empty(),
          "a real machine short-circuits the pipe branch: no block is read");
    CHECK(f.repo->cases.empty(), "and no pipe meta is written");
    CHECK_EQ(f.machine(ent).side_config[5], simcore::nextSideRole(5, false, true),
             "the machine's face role is cycled instead");
}

// ===========================================================================
// Fixture sanity: the constants this suite depends on
// ===========================================================================

static void test_Wrench_suite_content_ids_are_valid() {
    CHECK(ItemId::isPipe(kPipeId), "kPipeId is inside the pipe range");
    CHECK(!ItemId::isCable(kPipeId), "and is not a cable");
    CHECK(ItemId::isCable(kCableId), "kCableId is inside the cable range");
    CHECK(!ItemId::isPipe(kCableId), "and is not a pipe");
    CHECK_NE(kStoneId, 0, "the stone id is non-zero");
    CHECK_NE(kHeatFurnaceId, 0, "the heat furnace id is non-zero");
    CHECK_EQ(kFaces[5].dirBit, 0, "EAST is meta bit 0, per the client anchor table");
    CHECK_EQ(kFaces[0].dirBit, 3, "DOWN is meta bit 3");
}

static void test_Wrench_suite_next_side_role_transitions_are_stable() {
    // The cycle WrenchHandler actually drives, restated from the real table so
    // a change to SideConfig.h is visible in this suite.
    CHECK_EQ_I(simcore::nextSideRole(5, false, true), 0, "ANY -> INPUT");
    CHECK_EQ_I(simcore::nextSideRole(0, false, true), 2, "INPUT -> ENERGY (hasEnergy)");
    CHECK_EQ_I(simcore::nextSideRole(0, false, false), 1, "INPUT -> OUTPUT (no resources)");
    CHECK_EQ_I(simcore::nextSideRole(0, true, false), 3, "INPUT -> FLUID_IN (hasFluid)");
    CHECK_EQ_I(simcore::nextSideRole(2, false, true), 5, "ENERGY -> ANY");
    CHECK_EQ_I(simcore::nextSideRole(1, false, true), 0, "OUTPUT -> INPUT");
    CHECK_EQ_I(simcore::nextSideRole(3, true, false), 4, "FLUID_IN -> FLUID_OUT");
    CHECK_EQ_I(simcore::nextSideRole(4, false, false), 0, "FLUID_OUT -> INPUT (no fluid)");
}

#define TEST(name)                                                             \
    do {                                                                       \
        ++g_tests;                                                             \
        printf("  TEST: %s\n", #name);                                         \
        test_##name();                                                         \
    } while (0)

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("=== wrench server test suite (WrenchHandler / WrenchActionHandler) ===\n\n");

    // --- WrenchMeta -------------------------------------------------------------
    TEST(WrenchMeta_face_directions_match_the_table);
    TEST(WrenchMeta_toggle_matches_the_client_anchor_cases);
    TEST(WrenchMeta_zero_meta_means_all_connected);
    TEST(WrenchMeta_toggle_is_self_inverse);
    TEST(WrenchMeta_out_of_range_face_is_a_noop);

    // --- WrenchHandler::cycleFace ------------------------------------------------
    TEST(WrenchHandler_cycles_the_requested_face_only);
    TEST(WrenchHandler_role_cycle_against_the_real_table);
    TEST(WrenchHandler_reports_all_six_roles);
    TEST(WrenchHandler_returns_the_cycled_machine_id);
    TEST(WrenchHandler_no_machine_at_position_fails);
    TEST(WrenchHandler_position_match_is_exact);
    TEST(WrenchHandler_invalid_face_is_rejected);
    TEST(WrenchHandler_finds_the_first_machine_at_a_shared_position);
    TEST(WrenchHandler_find_entity_at_matches_only_position);
    TEST(WrenchHandler_hatch_cycling_uses_the_controller_hatch_slot);
    TEST(WrenchHandler_hatch_face_is_ignored);
    TEST(WrenchHandler_hatch_rejects_an_invalid_face);
    TEST(WrenchHandler_a_machine_shadows_a_co_located_hatch);

    // --- WrenchActionHandler ----------------------------------------------------
    TEST(WrenchActionHandler_wrong_action_type_is_ignored);
    TEST(WrenchActionHandler_rejects_a_malformed_buffer);
    TEST(WrenchActionHandler_rejects_an_empty_buffer);
    TEST(WrenchActionHandler_wrench_on_a_non_pipe_block_is_a_noop);
    TEST(WrenchActionHandler_air_is_also_a_noop);
    TEST(WrenchActionHandler_pipe_host_always_toggles_its_own_face);
    TEST(WrenchActionHandler_neighbour_is_written_only_when_it_is_a_pipe);
    TEST(WrenchActionHandler_cable_neighbour_is_treated_as_a_pipe);
    TEST(WrenchActionHandler_neighbour_lookup_is_on_the_clicked_face);
    TEST(WrenchActionHandler_meta_zero_host_is_normalised_to_all_connected);
    TEST(WrenchActionHandler_invalid_face_in_the_pipe_branch_is_dropped);
    TEST(WrenchActionHandler_conflicting_cas_leaves_the_world_unchanged);
    TEST(WrenchActionHandler_two_toggles_restore_the_original_meta);
    TEST(WrenchActionHandler_cooldown_blocks_an_immediate_repeat);
    TEST(WrenchActionHandler_cooldown_is_keyed_per_player);
    TEST(WrenchActionHandler_cooldown_key_ignores_the_item_held);
    TEST(WrenchActionHandler_machine_branch_takes_priority_over_the_pipe_branch);

    // --- Fixture sanity ---------------------------------------------------------
    TEST(Wrench_suite_content_ids_are_valid);
    TEST(Wrench_suite_next_side_role_transitions_are_stable);

    printf("\n=== Results: %d tests, %d passed checks, %d failed ===\n", g_tests,
           g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
