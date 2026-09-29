// Unit tests for fluid -> bucket filling by right-clicking a machine
// (issue gp-jmlq, design (b)).
//
//   src/game/actions/handlers/MachineInteractHandler.cpp
//     FluidBuckets (the fluid -> filled-bucket table) and tryFillBucket
//
// THE DESIGN BEING PINNED
// =========================
//
//   Right-click a machine that holds a fluid, while holding an empty bucket:
//   the bucket becomes the matching filled bucket and the machine gives up
//   1000 mB. A fluid with NO row in fluid_buckets.csv makes the click do
//   nothing, and the machine is NOT drained — the table is the only bridge
//   between a fluid as volume and a bucket as an item, and a fluid that is
//   absent from it has no bucket to conjure.
//
// WHY THIS FILE EXISTS SEPARATELY FROM test_block_actions.cpp
// ============================================================
//
// That file already pins the machine-interact ROUTE (the window opens, the
// energy type is the registry's, the left-click spin, the non-machine
// rejection) and the break/place/CAS contracts. It drives right-clicks with an
// EMPTY hand (held_item = 0) throughout, so it never once puts an empty bucket
// in the player's hand and therefore never reaches the new branch. These tests
// are about the bucket path only, and about the one property the whole
// ordering exists to guarantee:
//
//   THE MACHINE LOSES FLUID ONLY IF THE PLAYER ACTUALLY ENDS UP WITH A BUCKET.
//
// ============================================================================
// THE ORDERING, AND WHY IT IS THE WAY IT IS
// ============================================================================
//
// tryFillBucket() does, in this order:
//
//   1. every precondition, all read-only: right click, empty bucket in hand,
//      engine + inventory store + grant callback all non-null, an ECS entity
//      at (x,y,z), a FluidStorage on it with fluid_id != 0, amount >=
//      1000, maxOutput >= 1000, a table row for that fluid, and a bucket name
//      that hasName() says is a real item whose id is not 0;
//   2. consume the empty bucket out of the slot it was actually in;
//   3. removeFluid(1000), and VERIFY the return value was a whole 1000;
//   4. grant the filled bucket into that same slot.
//
// Steps 2-4 are ordered so a drained machine is always a machine whose bucket
// exists. The subtlety that forces it: ItemGiveCallback returns void, and the
// production lambda (SimCoreMessageHandler.cpp:160-162) DISCARDS
// PlayerInventoryStore::giveItem()'s bool, so "the inventory was full and the
// grant silently did nothing" is invisible to a caller that has already
// drained. Granting to an arbitrary free slot — the obvious implementation —
// is therefore exactly the version that destroys fluid on a full inventory.
//
// Targeting the freed slot removes the failure mode instead of papering over
// it: the empty bucket is gone from that slot before the grant runs, so
// giveItem's targeted branch (PlayerInventoryStore.cpp:99-118) finds item_id
// == 0 and always has somewhere to write, whether or not the rest of the
// inventory is full. The test below pins that with a completely full
// inventory.
//
// SetBlockAction carries `held_item` but NOT the slot it came from
// (core.fbs:142-153), so the hand cannot be addressed directly; the
// server-authoritative slots are scanned for the first empty-bucket stack,
// the same approach PlaceBlockHandler uses to charge for a placed block
// (PlaceBlockHandler.cpp:92-103). ctx.held_item is client-supplied, so it is
// treated as a claim to be confirmed against the server's own slots, not as
// the truth — a client claiming a bucket it does not hold gets nothing.
//
// ============================================================================
// HARNESS NOTE (mandatory, checked by the reviewer)
// ============================================================================
//
// GoogleTest is FORBIDDEN: absent from conanfile.txt, CI does not install
// libgtest-dev, and CI Release builds carry a global -Werror. A
// find_package(GTest QUIET) guard would silently UNREGISTER this test in CI.
// This file uses the project's own CHECK/TEST macros, mirrored from its
// siblings in this very directory.
//
// ============================================================================
// DETERMINISM
// ============================================================================
//
// No sockets, no threads, no sleeps, no display. The block repository double
// answers synchronously, and production dispatches setblock frames onto the
// sim main thread (SimCoreMessageHandler.cpp:241-246), so the ECS is touched
// on one thread and the tests are deterministic by construction. The real
// content tables (items.csv, fluid_buckets.csv) are loaded from disk, so a
// data change shows up as a test diff instead of a silent divergence.

#include <apps/simcore/Network/IEventPublisher.h>
#include <data/registry/ToolIds.h>
#include <engine/registry/ItemId.h>
#include <engine/sim/MachineRegistry.h>
#include <engine/sim/SimulationEngine.h>
#include <engine/sim/components/FluidStorage.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/Position.h>
#include <flatbuffers/flatbuffers.h>
#include <game/actions/ActionContext.h>
#include <game/actions/handlers/MachineInteractHandler.h>
#include <game/recipes/ItemRegistry.h>
#include <game/storage/IBlockRepository.h>
#include <game/storage/PlayerInventoryStore.h>
#include <game/world/BlockDrops.h>
#include <game/world/BlockTransforms.h>
#include <entt/entt.hpp>
#include "core_generated.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Project harness (src/engine/net/test/test.h), as in the sibling files.
// ---------------------------------------------------------------------------

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr,
                const char* msg = nullptr) {
  ++g_tests;
  if (cond) {
    ++g_passed;
  } else {
    ++g_failed;
    printf("  FAIL %s:%d: %s%s%s\n", file, line, expr, msg ? " -- " : "",
           msg ? msg : "");
  }
}

#define CHECK(cond, ...) \
  test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) \
  test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) \
  test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

#define TEST(name)   \
  do {               \
    printf("  TEST: %s\n", #name); \
    test_##name();   \
  } while (0)

namespace {

using namespace simcore;

// ---------------------------------------------------------------------------
// Content ids, all verified against the shipped tables.
// ---------------------------------------------------------------------------

// items.csv:234-238
constexpr uint16_t kWaterBucket = ItemId::pack("0:11111:0");
constexpr uint16_t kHydrogenBucket = ItemId::pack("0:11111:1");
constexpr uint16_t kSulfuricAcidBucket = ItemId::pack("0:11111:2");
constexpr uint16_t kEmptyBucket = ItemId::pack("0:11111:3");
constexpr uint16_t kCoolantBucket = ItemId::pack("0:11111:4");

// fluids.csv: item_id column — the SAME namespace FluidStorage::fluid_id
// carries (CreativeFluidSystem.h:35-42).
constexpr uint32_t kWaterFluid = ItemId::pack("1111:11:0");
constexpr uint32_t kSulfuricAcidFluid = ItemId::pack("1111:11:2");
constexpr uint32_t kHydrogenFluid = ItemId::pack("1111:11:4");
constexpr uint32_t kCoolantFluid = ItemId::pack("1111:11:5");
constexpr uint32_t kSteamFluid = ItemId::pack("1111:11:1");
// oil is 1111:11:58 (CreativeFluidSystem.h:41) and has NO row in
// fluid_buckets.csv on purpose — it is the fluid that must stay inert.
constexpr uint32_t kOilFluid = ItemId::pack("1111:11:58");

// machines.yaml: 1110:000:0 = heat_furnace.
constexpr uint16_t kFurnaceId = ItemId::pack("1110:000:0");
constexpr uint16_t kStoneId = ItemId::pack("0:0:1");

constexpr int32_t kBucketVolume = 1000;
static constexpr uint64_t kPlayerId = 7;
static constexpr uint32_t kRequestId = 4242;

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

struct AckRecord {
  uint8_t status = 0;
  int32_t x = 0, y = 0, z = 0;
  uint16_t block_id = 0;
  std::string reason;
  uint32_t request_id = 0;
};

struct DirectiveRecord {
  uint8_t directive = 0;
  uint16_t block_id = 0;
  int32_t x = 0, y = 0, z = 0;
  uint32_t request_id = 0;
};

struct GivenItem {
  uint64_t player_id = 0;
  uint16_t item_id = 0;
  uint8_t count = 0;
  int32_t target_slot = 0;
};

class RecordingPublisher : public IEventPublisher {
public:
  std::vector<AckRecord> acks;
  std::vector<DirectiveRecord> directives;
  int entity_update_count = 0;

  void publishBlockAck(uint8_t status, int32_t x, int32_t y, int32_t z,
                       uint16_t block_id, uint8_t, const char* reason,
                       uint32_t request_id = 0,
                       uint8_t = 1) override {
    acks.push_back(
        AckRecord{status, x, y, z, block_id, reason ? reason : "", request_id});
  }
  void publishBlockDirective(uint8_t directive, uint16_t block_id, int32_t x,
                             int32_t y, int32_t z, uint32_t request_id = 0,
                             uint8_t = 1) override {
    directives.push_back(
        DirectiveRecord{directive, block_id, x, y, z, request_id});
  }
  void publishBlockChangedEvent(int32_t, int32_t, int32_t, uint16_t, uint8_t,
                                uint32_t = 0, uint64_t = 0) override {}
  void publishBlockEntityUpdate(int32_t, int32_t, int32_t, uint16_t,
                                const std::vector<uint8_t>&, float, uint32_t,
                                EnergyType = EnergyType::ELECTRICITY,
                                uint32_t = 0, int = -1, float = 0.0f,
                                const std::vector<HatchUpdateData>* = nullptr,
                                double = -1.0, double = -1.0) override {
    ++entity_update_count;
  }
  void publishMachineSlotResponse(int32_t, int32_t, int32_t, uint16_t, bool,
                                  uint16_t, uint8_t, uint16_t,
                                  const char*) override {}
  void publishMachineConfigUpdatedEvent(int32_t, int32_t, int32_t,
                                        const std::array<uint8_t, 6>&) override {}
  void publishMultiblockCreated(uint64_t, int32_t, int32_t, int32_t,
                                uint16_t) override {}
  void publishMultiblockDestroyed(uint64_t) override {}
  void publishGridUpdate(int32_t, int32_t, int32_t,
                         const std::vector<RecipeManager::ItemStack>&) override {}

  void clear() {
    acks.clear();
    directives.clear();
    entity_update_count = 0;
  }
};

// A world double that only needs to answer getBlock, which the machine
// handler calls when the ECS has no entity yet.
class World : public IBlockRepository {
public:
  void set(int32_t x, int32_t y, int32_t z, uint16_t id) {
    cells_[key(x, y, z)] = id;
  }
  size_t blockReads() const { return block_reads_; }

  void setBlockCAS(int32_t, int32_t, int32_t, uint16_t, uint16_t, uint8_t,
                   SetBlockCASCallback callback) override {
    callback(CASResult{0, 0, 0});
  }
  void getBlock(int32_t x, int32_t y, int32_t z,
                GetBlockCallback callback) override {
    ++block_reads_;
    callback(BlockData{at(x, y, z), 0, 0});
  }

private:
  static uint64_t key(int32_t x, int32_t y, int32_t z) {
    return (static_cast<uint64_t>(x) & 0x1FFFFF) << 41 |
           (static_cast<uint64_t>(y) & 0xFFFFF) << 21 |
           (static_cast<uint64_t>(z) & 0x1FFFFF);
  }
  uint16_t at(int32_t x, int32_t y, int32_t z) const {
    auto it = cells_.find(key(x, y, z));
    return it == cells_.end() ? 0 : it->second;
  }
  std::unordered_map<uint64_t, uint16_t> cells_;
  size_t block_reads_ = 0;
};

// Installs a table for the lifetime of the guard, and restores the previous
// one. A test that points the singleton at a temporary file cannot leak it
// into the next test, which would make the suite order-dependent.
class FluidBucketsGuard {
public:
  explicit FluidBucketsGuard(const char* csv)
      : previous_(FluidBuckets::instance()) {
    owned_ = FluidBuckets::Load(csv);
    FluidBuckets::setInstance(owned_);
  }
  // Install an EMPTY table, for the "no table rows at all" case.
  FluidBucketsGuard()
      : previous_(FluidBuckets::instance()),
        owned_(FluidBuckets::Load("/nonexistent/fluid_buckets.csv")) {
    FluidBuckets::setInstance(owned_);
  }
  ~FluidBucketsGuard() {
    FluidBuckets::setInstance(previous_);
    delete owned_;
  }
  FluidBucketsGuard(const FluidBucketsGuard&) = delete;
  FluidBucketsGuard& operator=(const FluidBucketsGuard&) = delete;

private:
  FluidBuckets* previous_;
  FluidBuckets* owned_ = nullptr;
};

// BlockDrops / BlockTransforms are global singletons read by the handlers on
// this path. A null table is what a bare machine interaction already tolerates.
class TableGuard {
public:
  TableGuard() : drops_(BlockDrops::instance()),
                 transforms_(BlockTransforms::instance()) {
    BlockDrops::setInstance(nullptr);
    BlockTransforms::setInstance(nullptr);
  }
  ~TableGuard() {
    BlockDrops::setInstance(drops_);
    BlockTransforms::setInstance(transforms_);
  }
  TableGuard(const TableGuard&) = delete;
  TableGuard& operator=(const TableGuard&) = delete;

private:
  BlockDrops* drops_;
  BlockTransforms* transforms_;
};

// Loads the real item + fluid tables once, exactly as production main.cpp does.
// ItemRegistry is internally idempotent, so this is cheap on repeat calls.
void loadContent() {
  RecipeManager::ItemRegistry::instance().loadFromCSV(ITEMS_CSV);
}

struct Fixture {
  std::shared_ptr<RecordingPublisher> pub = std::make_shared<RecordingPublisher>();
  std::shared_ptr<World> world = std::make_shared<World>();
  std::shared_ptr<SimulationEngine> engine = std::make_shared<SimulationEngine>();
  std::shared_ptr<PlayerInventoryStore> inv = std::make_shared<PlayerInventoryStore>();
  std::unique_ptr<MachineRegistry> registry;
  std::vector<GivenItem> given;

  Fixture() {
    registry = MachineRegistry::LoadFromYaml(MACHINES_YAML);
    if (registry) engine->setMachineRegistry(registry.get());
    inv->initPlayer(kPlayerId);
  }

  // A machine entity at (x,y,z) holding `fluid`, built directly in the ECS.
  // Production creates these through onBlockChanged; the tests only need the
  // resulting components, and building them directly keeps this file
  // independent of block-change plumbing that has its own coverage.
  entt::entity addMachine(int32_t x, int32_t y, int32_t z, uint16_t machine_id,
                          const FluidStorage& fluid) {
    auto& reg = engine->reg();
    const entt::entity e = reg.create();
    reg.emplace<simcore::Position>(e, static_cast<uint32_t>(x),
                                   static_cast<uint32_t>(y),
                                   static_cast<uint32_t>(z));
    reg.emplace<simcore::MachineComponent>(
        e, machine_id, /*mb_id=*/0, static_cast<uint32_t>(x),
        static_cast<uint32_t>(y), static_cast<uint32_t>(z),
        /*machine_instance_id=*/0);
    reg.emplace<simcore::FluidStorage>(e, fluid);
    return e;
  }

  // A machine with NO fluid buffer at all.
  entt::entity addMachineWithoutFluid(int32_t x, int32_t y, int32_t z,
                                      uint16_t machine_id) {
    auto& reg = engine->reg();
    const entt::entity e = reg.create();
    reg.emplace<simcore::Position>(e, static_cast<uint32_t>(x),
                                   static_cast<uint32_t>(y),
                                   static_cast<uint32_t>(z));
    reg.emplace<simcore::MachineComponent>(
        e, machine_id, /*mb_id=*/0, static_cast<uint32_t>(x),
        static_cast<uint32_t>(y), static_cast<uint32_t>(z),
        /*machine_instance_id=*/0);
    return e;
  }

  void putInHand(uint16_t item_id, uint8_t count = 1, int slot = 0) {
    auto slots = inv->getSlots(kPlayerId);
    slots[static_cast<size_t>(slot)] = {item_id, count, 0};
    inv->setSlots(kPlayerId, slots);
  }

  void fillInventoryWith(uint16_t item_id) {
    auto slots = inv->getSlots(kPlayerId);
    for (auto& s : slots) s = {item_id, 1, 0};
    inv->setSlots(kPlayerId, slots);
  }

  int totalInInventory(uint16_t item) const {
    int n = 0;
    for (const auto& s : inv->getSlots(kPlayerId)) {
      if (s.item_id == item) n += s.count;
    }
    return n;
  }

  int32_t amountAt(entt::entity e) const {
    auto& reg = engine->reg();
    const auto* f = reg.try_get<simcore::FluidStorage>(e);
    return f ? f->amount : -1;
  }

  // Builds a real SetBlockAction flatbuffer. The finished buffer must outlive
  // the context, so the builder is owned here and only ONE context per fixture
  // may be alive at a time.
  ActionContext make(Protocol::PlayerActionType action, int32_t x, int32_t y,
                     int32_t z, uint16_t expected, uint16_t held,
                     bool grant_callback = true, bool with_store = true) {
    fbb_.Clear();
    Protocol::Vec3i pos(x, y, z);
    const auto off = Protocol::CreateSetBlockAction(
        fbb_, kPlayerId, action, &pos, expected, held, kRequestId, 0, held);
    fbb_.Finish(off);
    const auto* table =
        flatbuffers::GetRoot<Protocol::SetBlockAction>(fbb_.GetBufferPointer());
    ItemGiveCallback give;
    if (grant_callback) {
      give = [this](uint64_t pid, uint16_t item, uint8_t n, int32_t slot) {
        given.push_back({pid, item, n, slot});
        // The production lambda calls giveItem and drops the bool; the double
        // does the same so the test exercises the real grant semantics,
        // including the targeted-slot branch.
        if (inv) inv->giveItem(pid, item, n, slot);
      };
    }
    return ActionContext(table, world, pub, engine,
                         with_store ? inv : nullptr, nullptr, give, nullptr,
                         nullptr, nullptr);
  }

private:
  flatbuffers::FlatBufferBuilder fbb_;
};

// ===========================================================================
// The table itself
// ===========================================================================

// The shipped table loads, and every row it contains resolves to a real item
// whose id is neither 0 nor the empty bucket. This is what makes a wrong
// grant impossible rather than merely unlikely: a bucket name in the CSV that
// no item declares would silently become air.
void test_FluidBuckets_every_shipped_row_resolves_to_a_real_item() {
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  auto* loaded = FluidBuckets::instance();
  CHECK(loaded != nullptr, "the table is installed");
  if (!loaded) return;
  CHECK_EQ(loaded->size(), size_t(4),
           "fluid_buckets.csv declares exactly four rows");

  const auto& items = RecipeManager::ItemRegistry::instance();
  struct Row { uint32_t fluid; const char* name; uint16_t expect; };
  const Row rows[] = {
      {kWaterFluid, "water_bucket", kWaterBucket},
      {kSulfuricAcidFluid, "sulfuric_acid_bucket", kSulfuricAcidBucket},
      {kHydrogenFluid, "hydrogen_bucket", kHydrogenBucket},
      {kCoolantFluid, "coolant_bucket", kCoolantBucket},
  };
  for (const auto& r : rows) {
    const std::string* name = loaded->Get(r.fluid);
    CHECK(name != nullptr, "the row is present");
    if (!name) continue;
    CHECK_EQ(*name, std::string(r.name), "and it names the right bucket");
    CHECK(items.hasName(*name), "that name is a real item (hasName, not id != 0)");
    if (items.hasName(*name)) {
      const uint16_t id = items.nameToId(*name);
      CHECK_EQ(id, r.expect, "and it resolves to the items.csv id");
      CHECK_NE(id, uint16_t(0), "a bucket is never air");
      CHECK_NE(id, kEmptyBucket, "a filled bucket is never the empty bucket");
    }
  }
}

// The fluids that must stay inert are inert for the RIGHT reason: they are
// absent from the table, not present-and-broken.
void test_FluidBuckets_the_fluids_without_rows_really_have_none() {
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  auto* loaded = FluidBuckets::instance();
  if (!loaded) {
    CHECK(false, "the table is installed");
    return;
  }
  // oil, oxygen and steam deliberately have no row (the table's own header).
  CHECK(loaded->Get(kOilFluid) == nullptr, "oil has no bucket row");
  CHECK(loaded->Get(kSteamFluid) == nullptr, "steam has no bucket row");
  // A fluid id that does not exist at all.
  CHECK(loaded->Get(ItemId::pack("1111:11:250")) == nullptr,
        "an unknown fluid id has no bucket row");
}

// An unreadable table is an EMPTY table, never a null one. A null here would
// mean the click path has to null-check, and a wrong fix for that is a
// segfault on an ordinary machine click in a deployment that cannot read its
// own data directory.
void test_FluidBuckets_an_unreadable_table_is_empty_not_null() {
  FluidBucketsGuard broken("/nonexistent/fluid_buckets.csv");
  auto* loaded = FluidBuckets::instance();
  CHECK(loaded != nullptr, "a table that cannot be read is still a table");
  if (loaded) {
    CHECK_EQ(loaded->size(), size_t(0), "and it is empty");
    CHECK(loaded->Get(kWaterFluid) == nullptr,
          "so even water has no bucket, which makes the click inert");
  }
}

// A bucket name no item declares must be kept as a name, never collapsed into
// id 0. The handler re-checks with hasName() at click time; this pins that the
// table is what hands it a name rather than a number.
void test_FluidBuckets_an_unknown_bucket_name_stays_a_name() {
  const char* path = "/tmp/gtnh_fluid_buckets_bogus.csv";
  FILE* f = fopen(path, "w");
  if (f == nullptr) {
    CHECK(false, "the temporary table could be written");
    return;
  }
  fprintf(f, "# synthetic\n");
  fprintf(f, "1111:11:0,no_such_bucket\n");
  fclose(f);

  FluidBucketsGuard table(path);
  auto* loaded = FluidBuckets::instance();
  if (loaded) {
    const std::string* name = loaded->Get(kWaterFluid);
    CHECK(name != nullptr, "the row is kept");
    if (name) {
      CHECK_EQ(*name, std::string("no_such_bucket"),
               "the unknown name is preserved verbatim");
    }
  }
  remove(path);
}

// ===========================================================================
// The fill itself
// ===========================================================================

// THE happy path: water in the machine, empty bucket in hand, right click.
// The bucket is replaced by water_bucket and the machine gives up exactly
// 1000 mB.
void test_a_right_click_with_an_empty_bucket_fills_it_and_drains_1000() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(4, 5, 6, kFurnaceId);
  const entt::entity e = f.addMachine(
      4, 5, 6, kFurnaceId,
      FluidStorage{kWaterFluid, 5000, 8000, 1000, 1000});
  f.putInHand(kEmptyBucket, 1, 0);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 4, 5,
                             6, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  CHECK(h.canHandle(ctx), "a right click on a machine is claimed");
  h.handle(ctx);

  // Exactly 1000 mB out, of a 5000 mB buffer.
  CHECK_EQ(f.amountAt(e), 4000, "the machine gave up exactly one bucket");
  // One filled bucket, into the slot the empty one was in.
  CHECK_EQ(f.given.size(), size_t(1), "exactly one grant");
  if (f.given.size() == 1) {
    CHECK_EQ(f.given[0].item_id, kWaterBucket, "it is a water_bucket");
    CHECK_EQ(f.given[0].count, uint8_t(1), "one of them");
    CHECK_EQ(f.given[0].player_id, kPlayerId, "to the player who clicked");
    CHECK_EQ(f.given[0].target_slot, int32_t(0),
             "into the slot the empty bucket came from");
  }
  // The empty bucket is gone and the filled one is where the hand was.
  CHECK_EQ(f.totalInInventory(kEmptyBucket), 0, "no empty bucket is left");
  CHECK_EQ(f.totalInInventory(kWaterBucket), 1, "a water_bucket is in hand");
  // The grant is a replacement, not an extra: no second stack appeared.
  CHECK_EQ(f.pub->entity_update_count, 0,
           "the machine window was NOT opened — the click filled a bucket");
  CHECK_EQ(f.pub->acks.size(), size_t(0), "and no machine-interact ack went out");
}

// Every fluid with a table row fills with the matching bucket, and each is
// checked against items.csv rather than against a hand-written constant only.
void test_each_rowed_fluid_fills_with_its_own_bucket() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  struct Case { uint32_t fluid; uint16_t bucket; const char* name; };
  const Case cases[] = {
      {kWaterFluid, kWaterBucket, "water_bucket"},
      {kSulfuricAcidFluid, kSulfuricAcidBucket, "sulfuric_acid_bucket"},
      {kHydrogenFluid, kHydrogenBucket, "hydrogen_bucket"},
      {kCoolantFluid, kCoolantBucket, "coolant_bucket"},
  };
  for (const auto& c : cases) {
    Fixture f;
    f.world->set(1, 1, 1, kFurnaceId);
    f.addMachine(1, 1, 1, kFurnaceId,
                 FluidStorage{c.fluid, 2000, 8000, 1000, 1000});
    f.putInHand(kEmptyBucket, 1, 3);

    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 1, 1,
                               1, kFurnaceId, kEmptyBucket);
    MachineInteractHandler h;
    h.handle(ctx);

    CHECK_EQ(f.given.size(), size_t(1), c.name);
    if (f.given.size() == 1) {
      CHECK_EQ(f.given[0].item_id, c.bucket, c.name);
      CHECK_EQ(f.given[0].target_slot, int32_t(3), c.name);
    }
  }
}

// The ordering's reason to exist: a FULL inventory must not cost the player
// the fluid. The bucket is granted into the slot the empty bucket was removed
// from, so that slot is always free and the grant cannot be dropped — the case
// that would destroy fluid under the obvious "grant anywhere" implementation.
void test_a_full_inventory_does_not_cost_the_player_the_fluid() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(2, 2, 2, kFurnaceId);
  const entt::entity e = f.addMachine(
      2, 2, 2, kFurnaceId,
      FluidStorage{kWaterFluid, 3000, 8000, 1000, 1000});
  // Every slot full — and the hand holds the only empty bucket, at slot 0.
  f.fillInventoryWith(ItemId::pack("0:1111:2:1"));
  f.putInHand(kEmptyBucket, 1, 0);
  const int stone_before = f.totalInInventory(ItemId::pack("0:1111:2:1"));

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 2, 2,
                             2, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(1), "the grant still happened");
  CHECK_EQ(f.given[0].target_slot, int32_t(0), "into the freed hand slot");
  CHECK_EQ(f.amountAt(e), 2000, "so the drain and the grant agree");
  CHECK_EQ(f.totalInInventory(kWaterBucket), 1,
           "and the player really does hold the filled bucket");
  CHECK_EQ(f.totalInInventory(ItemId::pack("0:1111:2:1")), stone_before,
           "the other 39 slots are untouched");
}

// ===========================================================================
// The inert cases — every failure is a fall-through, never a partial drain
// ===========================================================================

// A non-empty bucket in hand. A water_bucket is not an empty_bucket, so the
// click is an ordinary machine click.
void test_a_filled_bucket_in_hand_is_inert() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(3, 3, 3, kFurnaceId);
  const entt::entity e = f.addMachine(
      3, 3, 3, kFurnaceId,
      FluidStorage{kWaterFluid, 5000, 8000, 1000, 1000});
  f.putInHand(kWaterBucket, 1, 0);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 3, 3,
                             3, kFurnaceId, kWaterBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(0), "nothing is granted");
  CHECK_EQ(f.amountAt(e), 5000, "and the machine is NOT drained");
  CHECK_EQ(f.pub->directives.size(), size_t(1),
           "the click still opens the machine window as usual");
}

// A fluid with no table row — oil, 1111:11:58 — is the case the table exists
// to make safe. The machine must keep its fluid, exactly.
void test_a_fluid_with_no_table_row_is_inert_and_not_drained() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(5, 5, 5, kFurnaceId);
  const entt::entity e = f.addMachine(
      5, 5, 5, kFurnaceId,
      FluidStorage{kOilFluid, 5000, 8000, 1000, 1000});
  f.putInHand(kEmptyBucket, 1, 0);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 5, 5,
                             5, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(0), "oil has no bucket, so nothing is granted");
  CHECK_EQ(f.amountAt(e), 5000, "and the machine is NOT drained");
  CHECK_EQ(f.totalInInventory(kEmptyBucket), 1, "the empty bucket is still held");
  CHECK_EQ(f.pub->directives.size(), size_t(1), "the window still opens");
}

// Too little fluid for a whole bucket. 999 mB must not become a bucket.
void test_a_machine_with_too_little_fluid_is_inert_and_not_drained() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(6, 6, 6, kFurnaceId);
  const entt::entity e = f.addMachine(
      6, 6, 6, kFurnaceId,
      FluidStorage{kWaterFluid, 999, 8000, 1000, 1000});
  f.putInHand(kEmptyBucket, 1, 0);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 6, 6,
                             6, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(0), "999 mB does not fill a bucket");
  CHECK_EQ(f.amountAt(e), 999, "and not one drop is drained");
  CHECK_EQ(f.totalInInventory(kEmptyBucket), 1, "the empty bucket is still held");
}

// A machine whose max_output is below one bucket would be clamped by
// removeFluid() into a PARTIAL drain. Refusing is the whole point: a partial
// drain with a whole bucket handed over is duplication, and a partial drain
// with no bucket destroys the fluid.
void test_a_max_output_below_one_bucket_is_inert_and_not_drained() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(7, 7, 7, kFurnaceId);
  const entt::entity e = f.addMachine(
      7, 7, 7, kFurnaceId,
      FluidStorage{kWaterFluid, 5000, 8000, 1000, 500});
  f.putInHand(kEmptyBucket, 1, 0);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 7, 7,
                             7, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(0), "a clamped removal is not a fill");
  CHECK_EQ(f.amountAt(e), 5000, "and the machine is NOT partially drained");
}

// A machine with no fluid buffer at all, and a buffer whose fluid_id is 0,
// are both "this machine holds no bucketable fluid".
void test_a_machine_with_no_fluid_client_is_inert() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  {
    Fixture f;
    f.world->set(8, 8, 8, kFurnaceId);
    f.addMachineWithoutFluid(8, 8, 8, kFurnaceId);
    f.putInHand(kEmptyBucket, 1, 0);
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 8, 8,
                               8, kFurnaceId, kEmptyBucket);
    MachineInteractHandler h;
    h.handle(ctx);
    CHECK_EQ(f.given.size(), size_t(0), "no FluidStorage, no bucket");
    CHECK_EQ(f.totalInInventory(kEmptyBucket), 1, "the bucket is still held");
  }
  {
    // fluid_id == 0 is an EMPTY buffer, not a fluid. Note maxOutput is 0 here
    // too, so this covers both readings of "nothing to give".
    Fixture f;
    f.world->set(9, 9, 9, kFurnaceId);
    f.addMachine(9, 9, 9, kFurnaceId, FluidStorage{0, 0, 8000, 1000, 1000});
    f.putInHand(kEmptyBucket, 1, 0);
    ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 9, 9,
                               9, kFurnaceId, kEmptyBucket);
    MachineInteractHandler h;
    h.handle(ctx);
    CHECK_EQ(f.given.size(), size_t(0), "an empty buffer gives no bucket");
  }
}

// A machine with no ECS entity. Production lazily creates one from ChunkStore,
// but only AFTER the fluid decision is made — and a machine that has no entity
// has no fluid buffer to read, so the fill cannot be attempted. The click must
// fall through to that lazy init, not crash and not drain.
void test_a_missing_machine_entity_is_inert() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(10, 10, 10, kFurnaceId);
  f.putInHand(kEmptyBucket, 1, 0);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 10,
                             10, 10, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(0), "no entity, no bucket, no crash");
  CHECK_EQ(f.totalInInventory(kEmptyBucket), 1, "the empty bucket is still held");
  CHECK_EQ(f.world->blockReads(), size_t(1),
           "the click fell through to the normal lazy-init path");
}

// THE case the ordering is built for: no grant callback means there is no way
// to put a bucket in the player's hand, so the machine must NOT be drained.
void test_no_grant_callback_means_no_drain() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(11, 11, 11, kFurnaceId);
  const entt::entity e = f.addMachine(
      11, 11, 11, kFurnaceId,
      FluidStorage{kWaterFluid, 5000, 8000, 1000, 1000});
  f.putInHand(kEmptyBucket, 1, 0);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 11, 11,
                             11, kFurnaceId, kEmptyBucket, /*grant_callback=*/false);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(0), "nothing could be granted");
  CHECK_EQ(f.amountAt(e), 5000, "and the machine is NOT drained");
  CHECK_EQ(f.totalInInventory(kEmptyBucket), 1, "the empty bucket is still held");
}

// No inventory store: the bucket cannot be taken from the hand, so nothing
// happens.
void test_no_inventory_store_is_inert() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(12, 12, 12, kFurnaceId);
  const entt::entity e = f.addMachine(
      12, 12, 12, kFurnaceId,
      FluidStorage{kWaterFluid, 5000, 8000, 1000, 1000});

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 12, 12,
                             12, kFurnaceId, kEmptyBucket,
                             /*grant_callback=*/true, /*with_store=*/false);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(0), "no store, no grant");
  CHECK_EQ(f.amountAt(e), 5000, "and the machine is NOT drained");
}

// The client claims an empty bucket it does not actually hold. ctx.held_item
// is client-supplied, so it is a claim to confirm, not the truth.
void test_a_claimed_bucket_that_is_not_in_the_inventory_is_inert() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(13, 13, 13, kFurnaceId);
  const entt::entity e = f.addMachine(
      13, 13, 13, kFurnaceId,
      FluidStorage{kWaterFluid, 5000, 8000, 1000, 1000});
  // The player's hand is empty: the frame lies about holding a bucket.

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 13, 13,
                             13, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(0), "a bucket nobody holds is not a bucket");
  CHECK_EQ(f.amountAt(e), 5000, "and the machine is NOT drained");
}

// A bucket name no item declares. The click must be inert, NOT a grant of air
// for 1000 mB — the gp-hmb0 failure mode, where an unresolved name became id 0
// and made the fill free.
void test_an_unresolvable_bucket_name_is_inert_and_not_drained() {
  loadContent();
  const char* path = "/tmp/gtnh_fluid_buckets_unknown.csv";
  FILE* f = fopen(path, "w");
  if (f == nullptr) {
    CHECK(false, "the temporary table could be written");
    return;
  }
  fprintf(f, "1111:11:0,definitely_not_an_item\n");
  fclose(f);

  FluidBucketsGuard table(path);
  TableGuard tables;
  Fixture fx;
  fx.world->set(14, 14, 14, kFurnaceId);
  const entt::entity e = fx.addMachine(
      14, 14, 14, kFurnaceId,
      FluidStorage{kWaterFluid, 5000, 8000, 1000, 1000});
  fx.putInHand(kEmptyBucket, 1, 0);

  ActionContext ctx = fx.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 14,
                              14, 14, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(fx.given.size(), size_t(0),
           "an unresolvable name grants nothing — certainly not air");
  CHECK_EQ(fx.amountAt(e), 5000, "and the machine is NOT drained");
  CHECK_EQ(fx.totalInInventory(kEmptyBucket), 1, "the empty bucket is still held");
  remove(path);
}

// An EMPTY table (a deployment that cannot read its data dir) makes every
// click inert rather than crashing.
void test_an_empty_table_makes_every_click_inert() {
  loadContent();
  FluidBucketsGuard empty;
  TableGuard tables;
  Fixture f;
  f.world->set(15, 15, 15, kFurnaceId);
  const entt::entity e = f.addMachine(
      15, 15, 15, kFurnaceId,
      FluidStorage{kWaterFluid, 5000, 8000, 1000, 1000});
  f.putInHand(kEmptyBucket, 1, 0);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 15, 15,
                             15, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(0), "no rows, no fill, no crash");
  CHECK_EQ(f.amountAt(e), 5000, "and the machine is NOT drained");
}

// A LEFT click with an empty bucket is the spin path, never a fill. The
// bucket-fill hook is right-click only.
void test_a_left_click_with_an_empty_bucket_never_fills() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(16, 16, 16, kFurnaceId);
  const entt::entity e = f.addMachine(
      16, 16, 16, kFurnaceId,
      FluidStorage{kWaterFluid, 5000, 8000, 1000, 1000});
  f.putInHand(kEmptyBucket, 1, 0);

  // heat_furnace does not opt into interact_on_left, so a left click is not
  // claimed by the machine handler at all — either way, it cannot fill.
  ActionContext ctx = f.make(Protocol::PlayerActionType_LEFT_MOUSE_CLICK, 16, 16,
                             16, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  CHECK(!h.canHandle(ctx), "a furnace does not opt into the left-click spin");
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(0), "a left click never fills a bucket");
  CHECK_EQ(f.amountAt(e), 5000, "and never drains the machine");
}

// A stack of buckets: one is consumed, the rest stay. The click is one bucket,
// not the whole stack.
void test_a_stack_of_buckets_loses_exactly_one() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(17, 17, 17, kFurnaceId);
  const entt::entity e = f.addMachine(
      17, 17, 17, kFurnaceId,
      FluidStorage{kWaterFluid, 5000, 8000, 1000, 1000});
  f.putInHand(kEmptyBucket, 5, 2);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 17, 17,
                             17, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.totalInInventory(kEmptyBucket), 4, "one bucket was consumed");
  CHECK_EQ(f.given.size(), size_t(1), "exactly one filled bucket was granted");
  CHECK_EQ(f.given[0].target_slot, int32_t(2), "into the stack's slot");
  CHECK_EQ(f.amountAt(e), 4000, "and exactly 1000 mB was drained");
}

// The exact boundary: 1000 mB is a whole bucket and must fill; the tests above
// already cover 999, so this pins the inclusive edge from the other side.
void test_exactly_one_bucket_of_fluid_does_fill() {
  loadContent();
  FluidBucketsGuard table(FLUID_BUCKETS_CSV);
  TableGuard tables;
  Fixture f;
  f.world->set(18, 18, 18, kFurnaceId);
  const entt::entity e = f.addMachine(
      18, 18, 18, kFurnaceId,
      FluidStorage{kWaterFluid, kBucketVolume, 8000, 1000, 1000});
  f.putInHand(kEmptyBucket, 1, 0);

  ActionContext ctx = f.make(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 18, 18,
                             18, kFurnaceId, kEmptyBucket);
  MachineInteractHandler h;
  h.handle(ctx);

  CHECK_EQ(f.given.size(), size_t(1), "exactly 1000 mB is enough");
  CHECK_EQ(f.amountAt(e), 0, "and the machine is left empty, not negative");
}

}  // namespace

// ---------------------------------------------------------------------------

int main() {
  printf("=== fluid bucket fill test suite ===\n\n");
  loadContent();

  TEST(FluidBuckets_every_shipped_row_resolves_to_a_real_item);
  TEST(FluidBuckets_the_fluids_without_rows_really_have_none);
  TEST(FluidBuckets_an_unreadable_table_is_empty_not_null);
  TEST(FluidBuckets_an_unknown_bucket_name_stays_a_name);

  TEST(a_right_click_with_an_empty_bucket_fills_it_and_drains_1000);
  TEST(each_rowed_fluid_fills_with_its_own_bucket);
  TEST(a_full_inventory_does_not_cost_the_player_the_fluid);
  TEST(a_filled_bucket_in_hand_is_inert);
  TEST(a_fluid_with_no_table_row_is_inert_and_not_drained);
  TEST(a_machine_with_too_little_fluid_is_inert_and_not_drained);
  TEST(a_max_output_below_one_bucket_is_inert_and_not_drained);
  TEST(a_machine_with_no_fluid_client_is_inert);
  TEST(a_missing_machine_entity_is_inert);
  TEST(no_grant_callback_means_no_drain);
  TEST(no_inventory_store_is_inert);
  TEST(a_claimed_bucket_that_is_not_in_the_inventory_is_inert);
  TEST(an_unresolvable_bucket_name_is_inert_and_not_drained);
  TEST(an_empty_table_makes_every_click_inert);
  TEST(a_left_click_with_an_empty_bucket_never_fills);
  TEST(a_stack_of_buckets_loses_exactly_one);
  TEST(exactly_one_bucket_of_fluid_does_fill);

  printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
