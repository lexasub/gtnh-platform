// Pipe fluid overlay logic tests (openspec add-pipe-fluid-overlay):
//   overlay gate (toggle off / non-fluid target / dense pipe), per-face mask
//   mapping over PipeMeshBuilder::detectConnections (the same call the chunk
//   mesh builder uses), authoritative display text (unknown/stale when no
//   current snapshot, never a fabricated zero), and a structural read-only
//   guard: the overlay path touches the store only through const FindAt and
//   carries plain data through FrameExt — nothing that could emit a request.
#include "Render/PipeFluidOverlay.h"

#include <common/ItemId.h>
#include <common/ResourceBufferStateCodec.h>

#include <RenderLib/Common/RenderAPI.h>

#include <cstdint>
#include <concepts>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

static int g_passed = 0, g_failed = 0;

static void test_check(bool cond, const char* file, int line, const char* expr,
                       const char* msg) {
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
#define PASS() do { ++g_passed; } while (0)

namespace {

const uint16_t kFluidPipe = pipeTypeToBlockId(PipeType::FLUID_PIPE);
const uint16_t kDenseFluidPipe = pipeTypeToBlockId(PipeType::DENSE_FLUID_PIPE);
const uint16_t kItemPipe = pipeTypeToBlockId(PipeType::ITEM_PIPE);
const uint16_t kCable = pipeTypeToBlockId(PipeType::CABLE_TIN);
const uint16_t kSteamItem = ItemId::pack("1111:11:1");
const uint16_t kMachine = ItemId::pack("1110:01:1");

// Minimal read-only world for detectConnections: only GetBlock/GetMeta exist,
// and block reads are counted to document that the overlay path only reads.
struct FakeWorld {
    std::unordered_map<uint64_t, uint16_t> blocks;
    std::unordered_map<uint64_t, uint8_t> metas;
    int block_reads = 0;

    static uint64_t Key(int32_t x, int32_t y, int32_t z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 42) |
               ((static_cast<uint64_t>(static_cast<uint32_t>(y)) & 0x1FFFFF) << 21) |
               (static_cast<uint64_t>(static_cast<uint32_t>(z)) & 0x1FFFFF);
    }
    void Set(int32_t x, int32_t y, int32_t z, uint16_t id, uint8_t meta = 0) {
        blocks[Key(x, y, z)] = id;
        metas[Key(x, y, z)] = meta;
    }
    uint16_t GetBlock(int32_t x, int32_t y, int32_t z) {
        ++block_reads;
        auto it = blocks.find(Key(x, y, z));
        return it != blocks.end() ? it->second : 0;
    }
    uint8_t GetMeta(int32_t x, int32_t y, int32_t z) const {
        auto it = metas.find(Key(x, y, z));
        return it != metas.end() ? it->second : 0;
    }
};

FaceMask DetectOver(FakeWorld& world, uint16_t targetId) {
    PipeMeshBuilder builder;
    return builder.detectConnections(
        0, 0, 0, blockIdToPipeType(targetId),
        [&](int32_t x, int32_t y, int32_t z) { return world.GetBlock(x, y, z); },
        [&](int32_t x, int32_t y, int32_t z) { return world.GetMeta(x, y, z); });
}

gtnh::common::ResourceBufferStateMsg MakeSteamState(uint64_t sequence,
                                                    uint64_t epoch = 7) {
    gtnh::common::ResourceBufferStateMsg state;
    state.owner_id = 42;
    state.port_id = 1;
    state.resource_kind = gtnh::common::ResourceKind::FLUID;
    state.resource_id = kSteamItem;
    state.amount = 300;
    state.capacity = 1000;
    state.rate = 16;
    state.epoch = epoch;
    state.sequence = sequence;
    state.x = 0;
    state.y = 0;
    state.z = 0;
    state.removed = false;
    return state;
}

bool ContainsDigit(const char* s) {
    for (; *s; ++s)
        if (*s >= '0' && *s <= '9') return true;
    return false;
}

}  // namespace

static void test_gate_toggle_off() {
    CHECK(!pipe_fluid_overlay::ShouldShowOverlay(false, true, kFluidPipe),
          "toggle off → no overlay even on a fluid pipe");
    CHECK(!pipe_fluid_overlay::ShouldShowOverlay(true, false, kFluidPipe),
          "no highlight → no overlay");
    CHECK(!pipe_fluid_overlay::ShouldShowOverlay(true, true, 0),
          "empty highlight → no overlay");
    PASS();
}

static void test_gate_non_fluid_targets() {
    CHECK(!pipe_fluid_overlay::ShouldShowOverlay(true, true, kItemPipe),
          "item pipe is not a fluid pipe");
    CHECK(!pipe_fluid_overlay::ShouldShowOverlay(true, true, kCable),
          "cable is out of scope for this overlay");
    CHECK(!pipe_fluid_overlay::ShouldShowOverlay(true, true, kSteamItem),
          "fluid item is not a pipe block");
    CHECK(!pipe_fluid_overlay::ShouldShowOverlay(true, true, kMachine),
          "machine block is not a pipe block");
    PASS();
}

static void test_gate_fluid_and_dense() {
    // Anchor the helper's enum offsets to the live registry order.  Constructing
    // both values through pipeTypeToBlockId alone would make a reordered enum
    // self-consistent and hide the production ID/type mismatch.
    CHECK(kFluidPipe == ItemId::pack("1111:10:0"),
          "fluid pipe uses registry offset 0");
    CHECK(kItemPipe == ItemId::pack("1111:10:1"),
          "item pipe uses registry offset 1");
    CHECK(pipeTypeToBlockId(PipeType::DENSE_ITEM_PIPE) ==
              ItemId::pack("1111:10:2"),
          "dense item pipe uses registry offset 2");
    CHECK(kDenseFluidPipe == ItemId::pack("1111:10:3"),
          "dense fluid pipe uses registry offset 3");

    CHECK(blockIdToPipeType(kFluidPipe) == PipeType::FLUID_PIPE,
          "ordinary fluid ID maps to FLUID_PIPE");
    CHECK(pipe_fluid_overlay::ShouldShowOverlay(true, true, kFluidPipe),
          "fluid pipe shows the overlay");
    CHECK(pipe_fluid_overlay::ShouldShowOverlay(true, true, kDenseFluidPipe),
          "dense fluid pipe shows the overlay");
    CHECK(blockIdToPipeType(kDenseFluidPipe) == PipeType::DENSE_FLUID_PIPE,
          "dense flag source: blockIdToPipeType round-trip");
    CHECK(pipe_fluid_overlay::IsFluidPipeType(PipeType::FLUID_PIPE) &&
              pipe_fluid_overlay::IsFluidPipeType(PipeType::DENSE_FLUID_PIPE),
          "exactly the two fluid pipe types pass IsFluidPipeType");
    PASS();
}

static void test_mask_connected_disconnected_faces() {
    // +X and +Y connected (same pipe / machine endpoint), the rest empty —
    // the spec scenario: quads on +X and +Y only.
    FakeWorld world;
    world.Set(1, 0, 0, kFluidPipe);
    world.Set(0, 1, 0, kFluidPipe);
    world.Set(0, 0, 1, kMachine);
    const FaceMask mask = DetectOver(world, kFluidPipe);
    CHECK((mask & FACE_EAST) != 0, "+X neighbour connects FACE_EAST");
    CHECK((mask & FACE_UP) != 0, "+Y neighbour connects FACE_UP");
    CHECK((mask & FACE_SOUTH) != 0, "machine neighbour connects FACE_SOUTH");
    CHECK((mask & (FACE_WEST | FACE_DOWN | FACE_NORTH)) == 0,
          "empty sides stay disconnected");

    bool connectable[6] = {};
    pipe_fluid_overlay::ConnectableFromMask(mask, connectable);
    CHECK(connectable[0] && connectable[2] && connectable[4],
          "cube indices 0(+X) 2(+Y) 4(+Z) are connected");
    CHECK(!connectable[1] && !connectable[3] && !connectable[5],
          "cube indices 1(-X) 3(-Y) 5(-Z) stay off");

    // Isolated pipe: nothing connected anywhere.
    FakeWorld isolated;
    const FaceMask emptyMask = DetectOver(isolated, kFluidPipe);
    bool none[6] = {};
    pipe_fluid_overlay::ConnectableFromMask(emptyMask, none);
    for (int i = 0; i < 6; ++i)
        CHECK(!none[i], "isolated pipe: no face connects");
    PASS();
}

static void test_mask_meta_gating() {
    // Same-type pipes on +X and -X, but the target's connection meta allows
    // only +X (bit 0) — the mask must honour the meta like mesh building does.
    FakeWorld world;
    world.Set(1, 0, 0, kFluidPipe);
    world.Set(-1, 0, 0, kFluidPipe);
    world.metas[FakeWorld::Key(0, 0, 0)] = 0x01;
    const FaceMask mask = DetectOver(world, kFluidPipe);
    CHECK((mask & FACE_EAST) != 0 && (mask & FACE_WEST) == 0,
          "meta bit 0 keeps only +X connected");

    // Legacy meta 0 = all faces connected: pipes on all six sides → 0x3F.
    FakeWorld legacy;
    legacy.Set(1, 0, 0, kFluidPipe);
    legacy.Set(-1, 0, 0, kFluidPipe);
    legacy.Set(0, 1, 0, kFluidPipe);
    legacy.Set(0, -1, 0, kFluidPipe);
    legacy.Set(0, 0, 1, kFluidPipe);
    legacy.Set(0, 0, -1, kFluidPipe);
    bool all[6] = {};
    pipe_fluid_overlay::ConnectableFromMask(DetectOver(legacy, kFluidPipe), all);
    for (int i = 0; i < 6; ++i)
        CHECK(all[i], "unset meta: all six faces connected");
    PASS();
}

static void test_state_text_unknown_without_snapshot() {
    ResourceBufferStateStore store;
    const char* what = "no snapshot at all";
    const auto* entry = store.FindAt(BlockPos{0, 0, 0});
    CHECK(entry == nullptr, what);
    char buf[128];
    pipe_fluid_overlay::FormatStateText(buf, sizeof(buf), entry);
    CHECK(std::string(buf).find("unknown/stale") != std::string::npos,
          "missing snapshot displays unknown/stale");
    CHECK(!ContainsDigit(buf), "missing snapshot never fabricates a number");
    PASS();
}

static void test_state_text_with_snapshot() {
    ItemRegistry::LoadFromCSV(DATA_DIR "/registry/items.csv");
    ResourceBufferStateStore store;
    CHECK(store.Enqueue(std::make_shared<std::vector<uint8_t>>(
              gtnh::common::SerializeResourceBufferState(MakeSteamState(1)))),
          "snapshot enqueued");
    store.ApplyPending();

    const auto* entry = store.FindAt(BlockPos{0, 0, 0});
    CHECK(entry != nullptr, "applied snapshot visible at the pipe position");
    char buf[128];
    pipe_fluid_overlay::FormatStateText(buf, sizeof(buf), entry);
    const std::string text = buf;
    CHECK(text.find("0x") != std::string::npos,
          "canonical packed resource id shown in hex");
    CHECK(text.find("steam") != std::string::npos,
          "name resolved at display time via ItemRegistry");
    CHECK(text.find("300 / 1000") != std::string::npos,
          "authoritative amount/capacity shown");
    PASS();
}

static void test_state_text_stale_after_removal() {
    ItemRegistry::LoadFromCSV(DATA_DIR "/registry/items.csv");
    ResourceBufferStateStore store;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(MakeSteamState(1))));
    store.ApplyPending();
    CHECK(store.FindAt(BlockPos{0, 0, 0}) != nullptr, "precondition: entry exists");

    auto removal = MakeSteamState(2);
    removal.removed = true;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(removal)));
    store.ApplyPending();

    const auto* entry = store.FindAt(BlockPos{0, 0, 0});
    CHECK(entry == nullptr, "removed port: no current snapshot");
    char buf[128];
    pipe_fluid_overlay::FormatStateText(buf, sizeof(buf), entry);
    CHECK(std::string(buf).find("unknown/stale") != std::string::npos,
          "stale state displays unknown/stale");
    CHECK(!ContainsDigit(buf), "stale state never fabricates a zero");
    PASS();
}

// ---------------------------------------------------------------------------
// Read-only structural guard: the overlay can only read. Pin the pieces a
// future change would have to break to smuggle a client→server request onto
// the overlay path (same spirit as the topic-guard test in
// ResourceBufferState_test.cpp).
// ---------------------------------------------------------------------------

template <typename Store>
concept ConstFindAtOnly = requires(const Store& s, const BlockPos& p) {
    { s.FindAt(p) } -> std::same_as<const typename Store::Entry*>;
};

static void test_read_only_guard() {
    // The overlay's only data source is a const read API — no mutating path.
    static_assert(ConstFindAtOnly<ResourceBufferStateStore>,
                  "overlay reads state only through const FindAt");

    // FrameExt carries plain data to the render thread: bools and a bool[6],
    // no callbacks/function pointers through which a request could be sent.
    static_assert(std::is_same_v<decltype(&renderlib::FrameExt::showPipeFluidOverlay),
                                 bool renderlib::FrameExt::*>);
    static_assert(std::is_same_v<decltype(renderlib::FrameExt::pipeFluidConnectable),
                                 bool[6]>);
    static_assert(std::is_same_v<decltype(&renderlib::FrameExt::pipeFluidIsDense),
                                 bool renderlib::FrameExt::*>);

    // The extracted helpers are plain free functions: no captured state,
    // no NetClient, nothing to send with.
    static_assert(std::is_same_v<decltype(&pipe_fluid_overlay::ShouldShowOverlay),
                                 bool (*)(bool, bool, uint16_t)>);
    static_assert(std::is_same_v<decltype(&pipe_fluid_overlay::ConnectableFromMask),
                                 void (*)(FaceMask, bool*)>);
    static_assert(std::is_same_v<decltype(&pipe_fluid_overlay::FormatStateText),
                                 void (*)(char*, size_t,
                                          const ResourceBufferStateStore::Entry*)>);

    // Runtime: the full overlay read path only reads the world (counted) and
    // never touches the store's queue (size stays 0 — no Enqueue happened).
    FakeWorld world;
    world.Set(1, 0, 0, kFluidPipe);
    ResourceBufferStateStore store;
    CHECK(pipe_fluid_overlay::ShouldShowOverlay(true, true, kFluidPipe),
          "overlay active for the guarded run");
    bool connectable[6] = {};
    pipe_fluid_overlay::ConnectableFromMask(DetectOver(world, kFluidPipe), connectable);
    CHECK(world.block_reads > 0, "overlay path reads the world");
    char buf[128];
    pipe_fluid_overlay::FormatStateText(
        buf, sizeof(buf), store.FindAt(BlockPos{0, 0, 0}));
    CHECK(store.size() == 0, "overlay read path enqueues nothing");
    PASS();
}

int main() {
    test_gate_toggle_off();
    test_gate_non_fluid_targets();
    test_gate_fluid_and_dense();
    test_mask_connected_disconnected_faces();
    test_mask_meta_gating();
    test_state_text_unknown_without_snapshot();
    test_state_text_with_snapshot();
    test_state_text_stale_after_removal();
    test_read_only_guard();

    fprintf(stderr, "%d/%d checks passed\n", g_passed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
