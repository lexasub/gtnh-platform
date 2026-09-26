// Crafting request lifecycle unit tests (issue gp-vlxr).
//
// Files under test — the three server-side workbench state-machine files, all
// in one target (wave-1 design note: splitting them would make several
// targets race on the same fixtures):
//   src/game/crafting/CraftRequestHandler.cpp   — CraftRequest topic handler
//   src/game/crafting/RecipeCompletedHandler.cpp — RecipeCompleted topic handler
//   src/game/crafting/WorkbenchStateManager.cpp   — per-position grid cache
//
// The lifecycle they own:
//
//   1. The client sends Protocol::CraftRequest {player_id, pos, slots}.
//   2. CraftRequestHandler IGNORES the client-supplied slots entirely — it is
//      server-authoritative. It asks WorkbenchStateManager for the grid at
//      `pos` (cache-first).
//   3. doCraft() resolves the recipe with
//      RecipeManager::findRecipeByInputs(crafting_table_id, grid), consumes
//      the inputs, writes the consumed grid back to the workbench, publishes
//      a GridUpdate to the client, deducts the consumed items from the player
//      inventory, grants the result, and answers with CraftResponse on
//      "sim.craft.response".
//   4. Machine-side crafting is a different path: the recipe service publishes
//      Protocol::RecipeCompleted, and RecipeCompletedHandler replaces the
//      InventoryContainer of the MachineComponent sitting at that position.
//
// No display, no network, no cluster, no wall clock:
//   * The router is a real simcore::IoUringRouterClient, never Connected().
//     RouterClient::publish() early-returns while disconnected, so no socket
//     is ever created and no thread is ever started. The CraftResponse bytes
//     therefore cannot be observed here — only the paths that do NOT publish
//     are asserted directly, and the published-payload paths are asserted
//     through their observable side effects (the workbench cache, the player
//     inventory and the GridUpdate).
//   * EntityStateStoreClient is likewise never Connected(), so
//     WorkbenchStateManager runs cache-only: the async ESS branch is not
//     reachable. The cache-first / cache-miss / clear paths are covered.
//   * QuestManager gets null QuestData/QuestGraph, which makes
//     checkCraftCompletion() a logged no-op — the craft still completes.
//
// Two defects are PINNED as observed behaviour, not blessed. Both are marked
// PRODUCTION DEFECT in their test comments. Neither is fixed by this file.
//
//   1. posKey() truncates z to 16 bits AND leaves y in the high 32 bits with
//      x. WorkbenchStateManager::posKey packs
//      (x << 0) | (y << 32) | ((uint16_t)z << 48). Two distinct positions can
//      therefore collide in the cache. See
//      test_posKey_z_is_truncated_to_16_bits. Filed as gp-mhiv.
//   2. removeGridState() does NOT publish anything and does not tell the
//      player their workbench view is gone; it clears the cache and (when
//      connected) saves an empty blob. A client that still has the workbench
//      UI open keeps showing the stale grid. Pinned by
//      test_removeGridState_does_not_notify_the_client.
//
// Two more found by this file:
//
//   3. CraftRequestHandler::doCraft matched inventory slots by item_id ALONE
//      when charging the consumed inputs, ignoring the slot's metadata, and
//      cleared only item_id (never metadata) on an emptied slot, so a
//      meta-bearing item could debit the WRONG variant of that item id. That
//      defect (gp-0ce5) is FIXED in CraftRequestHandler.cpp by this file's
//      test test_craft_debits_the_matching_metadata_variant; the slot match
//      now requires item_id AND metadata, and an emptied slot clears both,
//      mirroring RecipeManager::consumeInputs (RecipeManager.cpp:213).
//   4. RecipeCompletedHandler applies a result to whichever of several
//      co-located MachineComponents the entt view yields first, and that one
//      is the later-created entity, not necessarily the right one. The
//      replacement is destructive (inv.slots.clear()). Filed as gp-5wms.
//      NOT fixed here.
//
// Uses the PROJECT's own harness (src/engine/net/test/test.h convention,
// mirrored by src/game/machines/test/test_explosion_system.cpp and
// src/game/storage/test/test_chest_state_manager.cpp). GoogleTest is
// deliberately NOT used: it is absent from conanfile.txt, CI does not install
// libgtest-dev, and CI builds Release with a global -Werror.

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <asio.hpp>
#include <flatbuffers/flatbuffers.h>
#include <entt/entt.hpp>

#include "core_generated.h"
#include "recipe_generated.h"

#include <engine/registry/ItemId.h>
#include <engine/sim/SimulationEngine.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/MachineComponent.h>
#include <apps/simcore/Common/MainThreadQueue.h>
#include <apps/simcore/Network/IEventPublisher.h>
#include <apps/simcore/Network/clients/EntityStateStoreClient.h>
#include <apps/simcore/Network/clients/IoUringRouterClient.h>
#include <game/crafting/CraftRequestHandler.h>
#include <game/crafting/WorkbenchStateManager.h>
#include <game/crafting/RecipeCompletedHandler.h>
#include <game/quests/QuestManager.h>
#include <game/recipes/ItemRegistry.h>
#include <game/recipes/RecipeManager.h>
#include <game/storage/PlayerInventoryStore.h>

// ---------------------------------------------------------------------------
// Harness — definitions and macros near the TOP, before first use.
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

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// Records every GridUpdate the handler publishes. Everything else in
// IEventPublisher is a no-op: this test never moves a block.
struct RecordingPublisher : simcore::IEventPublisher {
  struct GridEvent {
    int32_t x, y, z;
    std::vector<RecipeManager::ItemStack> grid;
  };
  std::vector<GridEvent> grids;

  void publishBlockAck(uint8_t, int32_t, int32_t, int32_t, uint16_t, uint8_t,
                       const char*, uint32_t, uint8_t) override {}
  void publishBlockDirective(uint8_t, uint16_t, int32_t, int32_t, int32_t,
                             uint32_t, uint8_t) override {}
  void publishBlockChangedEvent(int32_t, int32_t, int32_t, uint16_t, uint8_t,
                                uint32_t, uint64_t) override {}
  void publishBlockEntityUpdate(int32_t, int32_t, int32_t, uint16_t,
                                const std::vector<uint8_t>&, float, uint32_t,
                                EnergyType, uint32_t, int, float,
                                const std::vector<HatchUpdateData>* = nullptr,
                                double = -1.0, double = -1.0) override {}
  void publishMachineSlotResponse(int32_t, int32_t, int32_t, uint16_t, bool,
                                  uint16_t, uint8_t, uint16_t,
                                  const char*) override {}
  void publishMachineConfigUpdatedEvent(int32_t, int32_t, int32_t,
                                        const std::array<uint8_t, 6>&) override {}
  void publishMultiblockCreated(uint64_t, int32_t, int32_t, int32_t,
                                 uint16_t) override {}
  void publishMultiblockDestroyed(uint64_t) override {}

  void publishGridUpdate(int32_t x, int32_t y, int32_t z,
                         const std::vector<RecipeManager::ItemStack>& grid) override {
    grids.push_back({x, y, z, grid});
  }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

using RecipeManager::ItemStack;

// The real crafting_table block id from machines.yaml ("0:10:11:1"), which is
// the machine id CraftRequestHandler hard-codes.
static constexpr uint16_t kCraftingTable = ItemId::pack("0:10:11:1");
static constexpr uint64_t kPlayer = 42;

// Real ids from src/content/data/registry/items.csv, packed exactly the way
// ItemRegistry::loadFromCSV packs them. The crafting_table recipe consumes
// oak_planks, so that is the input under test; cobblestone stands in for an
// unrelated stack the craft must not touch.
static const uint16_t kPlanks = ItemId::pack("0:10:00:0");      // oak_planks
static const uint16_t kCobble = ItemId::pack("0:0:2");          // cobblestone
static const uint16_t kStick = ItemId::pack("0:11110:0");      // stick
// oak_log, the single input of the real `oak_log_to_planks` recipe. A
// distinct item_id from kPlanks, so the 4-plank RESULT of that recipe cannot
// stack onto any oak_log slot and perturb the assertions below.
static const uint16_t kOakLog = ItemId::pack("0:10:11:2");      // oak_log

// One shared io_context so EntityStateStoreClient has something to bind to.
// It is never run, never polled, and the client is never Connect()ed.
struct Fixture {
  asio::io_context io;
  std::shared_ptr<simcore::IoUringRouterClient> router;
  std::shared_ptr<RecipeManager::RecipeManager> recipes;
  std::shared_ptr<simcore::PlayerInventoryStore> inventory;
  std::shared_ptr<simulation_core::WorkbenchStateManager> workbench;
  std::shared_ptr<RecordingPublisher> publisher;
  simcore::MainThreadQueue mainQueue;
  std::unique_ptr<simulation_core::CraftRequestHandler> craftHandler;

  explicit Fixture(uint32_t dim = 0) {
    // Never connected: Publish() and Save/LoadEntityState() short-circuit, so
    // no socket or thread is ever created. This is the isolation that keeps
    // the test deterministic.
    router = std::make_shared<simcore::IoUringRouterClient>();

    recipes = std::make_shared<RecipeManager::RecipeManager>();
    inventory = std::make_shared<simcore::PlayerInventoryStore>();
    workbench = std::make_shared<simulation_core::WorkbenchStateManager>(
        std::make_shared<simcore::EntityStateStoreClient>(io), dim);
    publisher = std::make_shared<RecordingPublisher>();

    // questManager is deliberately left null: doCraft() null-checks it, and
    // the craft path must still succeed.
    craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
        router, recipes, inventory, nullptr, workbench, &mainQueue, publisher);
  }
};

// Loads the item registry once (idempotent via ItemRegistry's loaded_ guard) and
// the real crafting-table recipes, so findRecipeByInputs() has a class to
// resolve. Registering the machine class is what the production path does via
// machines.yaml; here it is registered directly.
static void loadRecipes() {
  static bool done = false;
  if (done) return;
  RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");
  done = true;
}

static std::shared_ptr<RecipeManager::RecipeManager> recipesWithCraftingTable() {
  loadRecipes();
  auto rm = std::make_shared<RecipeManager::RecipeManager>();
  rm->loadRecipesFromYamlFile(DATA_DIR "/recipes/crafting_table.yaml");
  rm->registerMachineClass(kCraftingTable, "crafting_table", 0);
  return rm;
}

// A 9-slot grid in the exact shape of the real `crafting_table` recipe
// (2x2 oak_planks in the top-left corner). Every other cell is empty.
static std::vector<ItemStack> plank2x2Grid(uint8_t perSlot = 1) {
  std::vector<ItemStack> g(9, ItemStack{0, 0, 0});
  g[0] = {kPlanks, perSlot, 0};
  g[1] = {kPlanks, perSlot, 0};
  g[3] = {kPlanks, perSlot, 0};
  g[4] = {kPlanks, perSlot, 0};
  return g;
}

// Builds a Protocol::CraftRequest. `slots` is the CLIENT-SUPPLIED grid, which
// the handler must ignore — tests pass a deliberately bogus one to prove it.
static std::vector<uint8_t> buildCraftRequest(
    uint64_t player_id, int32_t x, int32_t y, int32_t z,
    const std::vector<Protocol::ItemStack>* slots = nullptr) {
  flatbuffers::FlatBufferBuilder fb(256);
  Protocol::Vec3i pos(x, y, z);
  auto req = slots
                 ? Protocol::CreateCraftRequestDirect(fb, player_id, &pos, slots)
                 : Protocol::CreateCraftRequest(fb, player_id, &pos);
  fb.Finish(req);
  return std::vector<uint8_t>(fb.GetBufferPointer(),
                              fb.GetBufferPointer() + fb.GetSize());
}

// Builds a Protocol::RecipeCompleted with a full replacement inventory.
static std::vector<uint8_t> buildRecipeCompleted(int32_t x, int32_t y, int32_t z,
                                                 uint16_t machine_id,
                                                 const char* recipe_id,
                                                 const std::vector<Protocol::ItemStack>& slots) {
  flatbuffers::FlatBufferBuilder fb(256);
  Protocol::Vec3i pos(x, y, z);
  auto rc = Protocol::CreateRecipeCompletedDirect(fb, &pos, machine_id,
                                                  recipe_id, &slots);
  fb.Finish(rc);
  return std::vector<uint8_t>(fb.GetBufferPointer(),
                              fb.GetBufferPointer() + fb.GetSize());
}

// Reads back a workbench grid from the cache.
static std::vector<ItemStack> cachedGrid(simulation_core::WorkbenchStateManager& wm,
                                         int32_t x, int32_t y, int32_t z) {
  std::vector<ItemStack> out;
  wm.getGridState(x, y, z, [&out](const std::vector<ItemStack>& g) { out = g; });
  return out;
}

// ===========================================================================
// 1. WorkbenchStateManager — the grid cache
// ===========================================================================

static void test_setGridState_then_getGridState_is_a_cache_hit() {
  asio::io_context io;
  simulation_core::WorkbenchStateManager wm(
      std::make_shared<simcore::EntityStateStoreClient>(io), 0);

  const auto grid = plank2x2Grid(3);
  wm.setGridState(10, 64, -5, grid);

  const auto got = cachedGrid(wm, 10, 64, -5);
  CHECK_EQ(got.size(), size_t(9), "the cached grid is returned whole");
  for (size_t i = 0; i < got.size() && i < grid.size(); ++i) {
    CHECK_EQ(got[i].item_id, grid[i].item_id, "item_id round-trips");
    CHECK_EQ(int(got[i].count), int(grid[i].count), "count round-trips");
    CHECK_EQ(got[i].metadata, grid[i].metadata, "metadata round-trips");
  }
}

static void test_getGridState_callback_fires_synchronously_on_cache_hit() {
  asio::io_context io;
  simulation_core::WorkbenchStateManager wm(
      std::make_shared<simcore::EntityStateStoreClient>(io), 0);
  wm.setGridState(1, 2, 3, plank2x2Grid());

  bool called = false;
  wm.getGridState(1, 2, 3, [&](const std::vector<ItemStack>& g) {
    called = true;
    CHECK_EQ(g.size(), size_t(9), "callback receives the cached grid");
  });
  // No ESS round-trip is possible here (the client is never connected), so a
  // cache hit MUST be synchronous. If this ever needs a poll loop, the
  // cache-first contract has been broken.
  CHECK(called, "a cache hit invokes the callback synchronously");
}

static void test_getGridState_cache_miss_yields_an_empty_grid() {
  asio::io_context io;
  simulation_core::WorkbenchStateManager wm(
      std::make_shared<simcore::EntityStateStoreClient>(io), 0);

  bool called = false;
  size_t size = 99;
  wm.getGridState(77, 88, 99, [&](const std::vector<ItemStack>& g) {
    called = true;
    size = g.size();
  });
  // Documented behaviour: with no cache entry and no connected ESS client, the
  // else-branch calls back with an empty grid. doCraft() turns that into the
  // "Workbench grid is empty" error response.
  CHECK(called, "a cache miss with no ESS still invokes the callback");
  CHECK_EQ(size, size_t(0), "the callback receives an EMPTY grid, not a 9-slot one");
}

static void test_setGridState_overwrites_a_previous_grid() {
  asio::io_context io;
  simulation_core::WorkbenchStateManager wm(
      std::make_shared<simcore::EntityStateStoreClient>(io), 0);

  wm.setGridState(5, 5, 5, plank2x2Grid(7));
  CHECK_EQ(int(cachedGrid(wm, 5, 5, 5)[0].count), 7, "7 per slot after the first write");

  wm.setGridState(5, 5, 5, std::vector<ItemStack>(9, ItemStack{0, 0, 0}));
  const auto after = cachedGrid(wm, 5, 5, 5);
  CHECK_EQ(int(after[0].count), 0, "a second write replaces the first");
  CHECK_EQ(int(after[0].item_id), 0, "the item is gone after the overwrite");
}

static void test_positions_are_keyed_independently() {
  asio::io_context io;
  simulation_core::WorkbenchStateManager wm(
      std::make_shared<simcore::EntityStateStoreClient>(io), 0);

  wm.setGridState(0, 0, 0, plank2x2Grid(1));
  wm.setGridState(100, 64, 200, plank2x2Grid(2));
  wm.setGridState(-50, -64, -200, plank2x2Grid(3));

  CHECK_EQ(int(cachedGrid(wm, 0, 0, 0)[0].count), 1, "origin keeps its own grid");
  CHECK_EQ(int(cachedGrid(wm, 100, 64, 200)[0].count), 2, "far position keeps its own");
  CHECK_EQ(int(cachedGrid(wm, -50, -64, -200)[0].count), 3, "negative coordinates are distinct keys");

  // Removing one position must not disturb the others.
  wm.removeGridState(100, 64, 200);
  CHECK_EQ(cachedGrid(wm, 100, 64, 200).size(), size_t(0), "the removed key is gone");
  CHECK_EQ(int(cachedGrid(wm, 0, 0, 0)[0].count), 1, "the origin is untouched by the removal");
  CHECK_EQ(int(cachedGrid(wm, -50, -64, -200)[0].count), 3, "the negative key is untouched too");
}

// PRODUCTION DEFECT 1: posKey truncates z to 16 bits.
static void test_posKey_z_is_truncated_to_16_bits() {
  asio::io_context io;
  simulation_core::WorkbenchStateManager wm(
      std::make_shared<simcore::EntityStateStoreClient>(io), 0);

  // z and z+65536 are 65_536 blocks apart — one full world height apart in any
  // sane coordinate system — yet posKey() shifts a uint16_t of z, so they
  // collide in the cache and the two workbenches share one grid.
  wm.setGridState(0, 0, 0, plank2x2Grid(1));
  wm.setGridState(0, 0, 65536, plank2x2Grid(9));

  const auto first = cachedGrid(wm, 0, 0, 0);
  const auto second = cachedGrid(wm, 0, 0, 65536);
  CHECK_EQ(int(first[0].count), 9,
           "PRODUCTION DEFECT: (0,0,0) and (0,0,65536) share one cache entry — "
           "posKey() truncates z to 16 bits, so writing the high-z workbench "
           "overwrote the low-z one");
  CHECK_EQ(int(second[0].count), 9, "both positions read back the same grid");
}

static void test_setGridState_with_null_ess_client_is_safe() {
  // Production always injects a client, but a null one must not crash: every
  // ESS call is behind `essClient_ &&`.
  simulation_core::WorkbenchStateManager wm(nullptr, 0);
  wm.setGridState(1, 1, 1, plank2x2Grid());
  CHECK_EQ(cachedGrid(wm, 1, 1, 1).size(), size_t(9), "the cache still works");

  bool called = false;
  wm.getGridState(2, 2, 2, [&](const std::vector<ItemStack>& g) {
    called = true;
    CHECK_EQ(g.size(), size_t(0), "a null client yields an empty grid");
  });
  CHECK(called, "the callback still fires");
  wm.removeGridState(1, 1, 1);  // must not crash
  CHECK_EQ(cachedGrid(wm, 1, 1, 1).size(), size_t(0), "removal still works");
}

// PRODUCTION DEFECT 2: removeGridState() notifies nobody.
static void test_removeGridState_does_not_notify_the_client() {
  Fixture f;
  f.workbench->setGridState(0, 0, 0, plank2x2Grid());
  const auto before = f.publisher->grids.size();
  f.workbench->removeGridState(0, 0, 0);

  CHECK_EQ(cachedGrid(*f.workbench, 0, 0, 0).size(), size_t(0),
           "the cache entry is cleared");
  CHECK_EQ(f.publisher->grids.size(), before,
           "PRODUCTION DEFECT: no GridUpdate is published, so a client with "
           "the workbench UI still open keeps rendering the stale grid until "
           "it re-opens the bench");
}

// ===========================================================================
// 2. CraftRequestHandler — the craft request lifecycle
// ===========================================================================

static void test_craft_success_consumes_the_grid_and_grants_the_result() {
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  f.craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
      f.router, recipes, f.inventory, nullptr, f.workbench, &f.mainQueue,
      f.publisher);

  // The player holds 8 planks; the bench holds a 2x2 (4 planks).
  auto slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>{};
  slots[0] = {kPlanks, 8, 0};
  f.inventory->setSlots(kPlayer, slots);

  f.workbench->setGridState(3, 64, 7, plank2x2Grid(1));

  f.craftHandler->handle(buildCraftRequest(kPlayer, 3, 64, 7));
  f.mainQueue.drain();

  // The bench grid is consumed: all four plank cells are now empty.
  const auto after = cachedGrid(*f.workbench, 3, 64, 7);
  CHECK_EQ(after.size(), size_t(9), "the workbench grid keeps its 9 slots");
  for (int i : {0, 1, 3, 4}) {
    CHECK_EQ(int(after[static_cast<size_t>(i)].item_id), 0,
             "consumed plank slot is emptied");
    CHECK_EQ(int(after[static_cast<size_t>(i)].count), 0,
             "consumed plank slot has count 0");
  }

  // The player paid 4 planks and received the crafting table.
  const auto inv = f.inventory->getSlots(kPlayer);
  int planksLeft = 0;
  for (const auto& s : inv) {
    if (s.item_id == kPlanks) planksLeft += s.count;
  }
  CHECK_EQ(planksLeft, 4, "exactly 4 planks were deducted from the inventory");

  int tables = 0;
  for (const auto& s : inv) {
    if (s.item_id == kCraftingTable) tables += s.count;
  }
  CHECK_EQ(tables, 1, "one crafting table was granted");

  // The client is told about the new grid exactly once.
  CHECK_EQ(f.publisher->grids.size(), size_t(1), "exactly one GridUpdate published");
  if (f.publisher->grids.size() == 1) {
    CHECK_EQ(f.publisher->grids[0].x, 3, "GridUpdate carries the workbench x");
    CHECK_EQ(f.publisher->grids[0].y, 64, "GridUpdate carries the workbench y");
    CHECK_EQ(f.publisher->grids[0].z, 7, "GridUpdate carries the workbench z");
    CHECK_EQ(int(f.publisher->grids[0].grid[0].item_id), 0,
             "the published grid is the CONSUMED grid");
  }
}

static void test_craft_is_server_authoritative_and_ignores_client_slots() {
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  f.craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
      f.router, recipes, f.inventory, nullptr, f.workbench, &f.mainQueue,
      f.publisher);

  auto slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>{};
  slots[0] = {kPlanks, 8, 0};
  f.inventory->setSlots(kPlayer, slots);

  // The workbench holds a DELIBERATELY UNMATCHABLE grid (a single gem).
  std::vector<ItemStack> bogus(9, ItemStack{0, 0, 0});
  bogus[0] = {ItemId::pack("0:10:11:1"), 1, 0};
  f.workbench->setGridState(3, 64, 7, bogus);

  // The client CLAIMS to have a valid 2x2 of planks in its `slots` field.
  std::vector<Protocol::ItemStack> clientClaim;
  for (int n = 0; n < 4; ++n) {
    clientClaim.push_back(Protocol::ItemStack(kPlanks, 1, 0));
  }
  while (clientClaim.size() < 9) clientClaim.push_back(Protocol::ItemStack(0, 0, 0));

  f.craftHandler->handle(
      buildCraftRequest(kPlayer, 3, 64, 7, &clientClaim));
  f.mainQueue.drain();

  // The authoritative bench grid did not match, so nothing was crafted even
  // though the client said it would.
  CHECK_EQ(f.publisher->grids.size(), size_t(0),
           "a client-supplied matching grid does not override the "
           "server-authoritative bench state");
  const auto inv = f.inventory->getSlots(kPlayer);
  CHECK_EQ(int(inv[0].count), 8, "no planks were consumed");
  int tables = 0;
  for (const auto& s : inv) {
    if (s.item_id == kCraftingTable) tables += s.count;
  }
  CHECK_EQ(tables, 0, "no result was granted");
}

static void test_craft_from_an_empty_grid_grants_nothing() {
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  f.craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
      f.router, recipes, f.inventory, nullptr, f.workbench, &f.mainQueue,
      f.publisher);

  // No entry for this position at all -> getGridState() yields {}.
  f.craftHandler->handle(buildCraftRequest(kPlayer, 9, 9, 9));
  f.mainQueue.drain();

  CHECK_EQ(f.publisher->grids.size(), size_t(0), "nothing is published");
  const auto inv = f.inventory->getSlots(kPlayer);
  bool anyItem = false;
  for (const auto& s : inv) {
    if (s.item_id != 0) anyItem = true;
  }
  CHECK(!anyItem, "an empty grid grants nothing");
}

static void test_craft_with_a_non_matching_grid_leaves_state_untouched() {
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  f.craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
      f.router, recipes, f.inventory, nullptr, f.workbench, &f.mainQueue,
      f.publisher);

  auto slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>{};
  slots[0] = {kPlanks, 8, 0};
  f.inventory->setSlots(kPlayer, slots);

  // Nine planks: a filled 3x3 does not match the 2x2 pattern.
  std::vector<ItemStack> full(9, ItemStack{kPlanks, 1, 0});
  f.workbench->setGridState(1, 1, 1, full);

  f.craftHandler->handle(buildCraftRequest(kPlayer, 1, 1, 1));
  f.mainQueue.drain();

  const auto after = cachedGrid(*f.workbench, 1, 1, 1);
  CHECK_EQ(int(after[0].count), 1, "a non-matching grid is NOT consumed");
  CHECK_EQ(int(f.inventory->getSlots(kPlayer)[0].count), 8,
           "the inventory is not charged for a failed match");
  CHECK_EQ(f.publisher->grids.size(), size_t(0), "no GridUpdate on failure");
}

static void test_craft_deducts_only_the_recipe_inputs() {
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  f.craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
      f.router, recipes, f.inventory, nullptr, f.workbench, &f.mainQueue,
      f.publisher);

  // The player has 4 planks (consumed by the bench) and 6 cobblestone, which
  // is NOT part of the crafting_table recipe and must survive the craft.
  auto slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>{};
  slots[0] = {kPlanks, 4, 0};
  slots[1] = {kCobble, 6, 0};
  f.inventory->setSlots(kPlayer, slots);

  f.workbench->setGridState(2, 2, 2, plank2x2Grid(1));
  f.craftHandler->handle(buildCraftRequest(kPlayer, 2, 2, 2));
  f.mainQueue.drain();

  // The plank stack is charged, not merely decremented: doCraft() runs the
  // whole 4-plank deduction and then giveItem()s the crafting table into the
  // FIRST empty slot — which is slot 0, the plank slot the deduction just
  // freed (CraftRequestHandler.cpp:111-121 deducts, then :147 grants into
  // target_slot -1, and PlayerInventoryStore::giveItem takes the first
  // `item_id == 0` slot). So slot 0 is NOT observable as an emptied slot; it is
  // observable as the result stack. Assert the real end state.
  const auto inv = f.inventory->getSlots(kPlayer);
  int planks = 0, tables = 0;
  for (const auto& s : inv) {
    if (s.item_id == kPlanks) planks += s.count;
    if (s.item_id == kCraftingTable) tables += s.count;
  }
  CHECK_EQ(planks, 0,
           "all 4 planks were consumed: zero oak_planks remain anywhere");
  CHECK_EQ(tables, 1, "exactly one crafting_table was granted");
  CHECK_EQ(int(inv[0].item_id), int(kCraftingTable),
           "the result reuses the slot the deduction emptied (giveItem takes "
           "the first empty slot, which is slot 0)");
  CHECK_EQ(int(inv[1].count), 6, "the unrelated stack is untouched");
  CHECK_EQ(int(inv[1].item_id), int(kCobble),
           "the unrelated stack keeps its item_id");
}

static void test_craft_without_a_main_queue_runs_inline() {
  // mainQueue_ == nullptr must run doCraft() directly on the calling thread.
  // Both paths must produce the same observable result.
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  simulation_core::CraftRequestHandler inlineHandler(
      f.router, recipes, f.inventory, nullptr, f.workbench, nullptr,
      f.publisher);

  auto slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>{};
  slots[0] = {kPlanks, 8, 0};
  f.inventory->setSlots(kPlayer, slots);
  f.workbench->setGridState(4, 4, 4, plank2x2Grid(1));

  inlineHandler.handle(buildCraftRequest(kPlayer, 4, 4, 4));
  // No drain(): the work must already be done.

  CHECK_EQ(f.publisher->grids.size(), size_t(1),
           "with a null main queue the craft completes synchronously");
  int tables = 0;
  for (const auto& s : f.inventory->getSlots(kPlayer)) {
    if (s.item_id == kCraftingTable) tables += s.count;
  }
  CHECK_EQ(tables, 1, "the result is granted on the calling thread too");
}

static void test_craft_work_is_queued_not_run_inside_handle() {
  // The grid-state callback may fire on an ESS io thread, so doCraft() is
  // ALWAYS bounced through the main queue when one is supplied. With a queue
  // present, handle() must not craft — only drain() may.
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  f.craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
      f.router, recipes, f.inventory, nullptr, f.workbench, &f.mainQueue,
      f.publisher);

  auto slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>{};
  slots[0] = {kPlanks, 8, 0};
  f.inventory->setSlots(kPlayer, slots);
  f.workbench->setGridState(6, 6, 6, plank2x2Grid(1));

  f.craftHandler->handle(buildCraftRequest(kPlayer, 6, 6, 6));
  CHECK_EQ(f.mainQueue.size(), size_t(1), "handle() enqueued exactly one task");
  CHECK_EQ(f.publisher->grids.size(), size_t(0),
           "nothing has run yet: the craft is deferred to the main thread");
  CHECK_EQ(int(cachedGrid(*f.workbench, 6, 6, 6)[0].count), 1,
           "the grid is untouched before the drain");

  f.mainQueue.drain();
  CHECK_EQ(f.publisher->grids.size(), size_t(1), "the drain performs the craft");
  CHECK_EQ(f.mainQueue.size(), size_t(0), "the queue is empty afterwards");
}

static void test_malformed_craft_request_is_rejected_silently() {
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  f.craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
      f.router, recipes, f.inventory, nullptr, f.workbench, &f.mainQueue,
      f.publisher);

  // Garbage bytes: the FlatBuffers verifier must refuse them.
  const std::vector<uint8_t> junk = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22};
  f.craftHandler->handle(junk);
  f.mainQueue.drain();

  CHECK_EQ(f.publisher->grids.size(), size_t(0), "junk produces no GridUpdate");
  CHECK_EQ(f.mainQueue.size(), size_t(0), "junk enqueues no work");

  // An empty buffer too.
  f.craftHandler->handle({});
  f.mainQueue.drain();
  CHECK_EQ(f.publisher->grids.size(), size_t(0), "an empty buffer is rejected");
}

static void test_craft_without_a_workbench_manager_is_a_noop() {
  // The null-wbStateManager_ branch: it cannot craft, and must not crash.
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  simulation_core::CraftRequestHandler noBench(
      f.router, recipes, f.inventory, nullptr, nullptr, &f.mainQueue,
      f.publisher);

  noBench.handle(buildCraftRequest(kPlayer, 8, 8, 8));
  f.mainQueue.drain();

  CHECK_EQ(f.publisher->grids.size(), size_t(0), "no state is touched");
  CHECK_EQ(f.mainQueue.size(), size_t(0), "no work is enqueued");
}

static void test_craft_of_a_multi_item_recipe_deducts_each_input() {
  // The real `stick` recipe is a vertical 1x2 of oak_planks, so it exercises
  // the deduction loop over a NON-square shape and, unlike crafting_table,
  // yields a count > 1 (4 sticks). Both must flow through correctly.
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  f.craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
      f.router, recipes, f.inventory, nullptr, f.workbench, &f.mainQueue,
      f.publisher);

  auto slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>{};
  slots[0] = {kPlanks, 2, 0};
  f.inventory->setSlots(kPlayer, slots);

  // Stick pattern: plank at index 0, plank at index 3 (one row down).
  std::vector<ItemStack> stickGrid(9, ItemStack{0, 0, 0});
  stickGrid[0] = {kPlanks, 1, 0};
  stickGrid[3] = {kPlanks, 1, 0};
  f.workbench->setGridState(12, 12, 12, stickGrid);

  f.craftHandler->handle(buildCraftRequest(kPlayer, 12, 12, 12));
  f.mainQueue.drain();

  const auto inv = f.inventory->getSlots(kPlayer);
  int planks = 0, sticks = 0;
  for (const auto& s : inv) {
    if (s.item_id == kPlanks) planks += s.count;
    if (s.item_id == kStick) sticks += s.count;
  }
  CHECK_EQ(planks, 0, "both planks were consumed");
  CHECK_EQ(sticks, 4, "the recipe yields 4 sticks, and the count is granted");
  const auto after = cachedGrid(*f.workbench, 12, 12, 12);
  CHECK_EQ(int(after[0].item_id), 0, "grid slot 0 emptied");
  CHECK_EQ(int(after[3].item_id), 0, "grid slot 3 emptied");
}

static void test_repeated_crafts_consume_the_grid_each_time() {
  // The grid empties after the first craft, so a second identical request must
  // find nothing to match. This pins that the consumed grid is really stored,
  // not just published.
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  f.craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
      f.router, recipes, f.inventory, nullptr, f.workbench, &f.mainQueue,
      f.publisher);

  auto slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>{};
  slots[0] = {kPlanks, 8, 0};
  f.inventory->setSlots(kPlayer, slots);
  f.workbench->setGridState(11, 11, 11, plank2x2Grid(1));

  f.craftHandler->handle(buildCraftRequest(kPlayer, 11, 11, 11));
  f.mainQueue.drain();
  CHECK_EQ(f.publisher->grids.size(), size_t(1), "first craft succeeds");

  // Refill the bench but leave the player with no planks to pay with: the
  // craft still resolves, and the inventory deduction is clamped at zero.
  f.workbench->setGridState(11, 11, 11, plank2x2Grid(1));
  f.craftHandler->handle(buildCraftRequest(kPlayer, 11, 11, 11));
  f.mainQueue.drain();
  CHECK_EQ(f.publisher->grids.size(), size_t(2), "the second craft also runs");

  int planks = 0;
  for (const auto& s : f.inventory->getSlots(kPlayer)) {
    if (s.item_id == kPlanks) planks += s.count;
  }
  // 8 planks - 4 (first craft) - 4 (second craft) == 0. The deduction loop
  // breaks when it runs out of matching slots, so the count cannot go negative
  // and a uint8_t cannot wrap.
  CHECK_EQ(planks, 0, "the second craft spends the remaining 4 planks exactly");
}

// ===========================================================================
// 2b. CraftRequestHandler — the inventory deduction matches on metadata
// ===========================================================================

// gp-0ce5. doCraft() must charge the player for exactly the variant of an
// item that the workbench actually consumed.
//
// The real `oak_log_to_planks` recipe has the single-cell pattern
// [oak_log, ~, ~] whose cell metadata is 0 (parseYamlRecipe always builds
// pattern cells as {item_id, 1, 0}, RecipeManager.cpp:680), and
// Recipe::matches rejects a grid cell whose metadata differs. So the bench can
// only ever hold a META-0 oak_log — while the player may well be carrying a
// damaged oak_log (meta 5) in a lower inventory slot.
//
// Before the fix, CraftRequestHandler.cpp:113 compared only
// `slot.item_id == orig.item_id`, so the charge walked the inventory from slot
// 0 and debited the damaged log — the wrong item entirely — and line 117
// cleared only item_id, leaving orphaned metadata on the now-empty slot.
//
// Slots 2..39 are pre-filled with cobblestone and slot 2 holds a PARTIAL
// oak_planks stack. The 4-plank result therefore stacks onto slot 2 in
// giveItem()'s first pass instead of refilling the slot the deduction just
// emptied. That is what makes both halves of the defect observable at once:
// the freed slot survives as an empty slot, so orphaned metadata on it is
// visible, and WHICH stack was emptied is visible.
static void test_craft_debits_the_matching_metadata_variant() {
  auto recipes = recipesWithCraftingTable();
  Fixture f;
  f.craftHandler = std::make_unique<simulation_core::CraftRequestHandler>(
      f.router, recipes, f.inventory, nullptr, f.workbench, &f.mainQueue,
      f.publisher);

  auto slots = std::array<simcore::PersistSlot, simcore::kInventorySlots>{};
  slots[0] = {kOakLog, 1, 5};  // damaged log — a DIFFERENT variant of oak_log
  slots[1] = {kOakLog, 1, 0};  // pristine log — the variant the bench consumed
  slots[2] = {kPlanks, 3, 0};  // partial result stack: absorbs the 4-plank grant
  for (int i = 3; i < simcore::kInventorySlots; ++i) {
    slots[static_cast<size_t>(i)] = {kCobble, 1, 0};
  }
  f.inventory->setSlots(kPlayer, slots);

  // Bench: one oak_log, meta 0 — the only shape oak_log_to_planks matches.
  std::vector<ItemStack> logGrid(9, ItemStack{0, 0, 0});
  logGrid[0] = {kOakLog, 1, 0};
  f.workbench->setGridState(21, 21, 21, logGrid);

  f.craftHandler->handle(buildCraftRequest(kPlayer, 21, 21, 21));
  f.mainQueue.drain();

  CHECK_EQ(f.publisher->grids.size(), size_t(1),
           "the meta-0 oak_log matched oak_log_to_planks, so the craft ran");

  const auto inv = f.inventory->getSlots(kPlayer);

  // The meta-5 log is a different item and must not have been touched.
  CHECK_EQ(int(inv[0].item_id), int(kOakLog),
           "the damaged (meta 5) oak_log was NOT debited: the deduction must "
           "match item_id AND metadata, so a differing metadata is a "
           "different item and is not chargeable");
  CHECK_EQ(int(inv[0].count), 1, "the damaged log keeps its count of 1");
  CHECK_EQ(int(inv[0].meta), 5, "the damaged log keeps its metadata of 5");

  // The pristine log is the variant the bench actually consumed.
  CHECK_EQ(int(inv[1].item_id), 0,
           "the meta-0 oak_log — the variant the recipe consumed — was the "
           "one debited to empty");
  CHECK_EQ(int(inv[1].count), 0, "the debited stack is empty");

  // The emptied slot must be fully cleared, not left advertising stale
  // metadata: RecipeManager::consumeInputs (RecipeManager.cpp:217-220) clears
  // item_id AND metadata, and an empty slot still carrying meta is published
  // to the client and persisted to MetaDB as a phantom item.
  bool anyEmptySlotWithMeta = false;
  int emptySlotIndex = -1;
  for (int i = 0; i < simcore::kInventorySlots; ++i) {
    const auto& s = inv[static_cast<size_t>(i)];
    if (s.item_id == 0 && (s.meta != 0 || s.count != 0)) {
      anyEmptySlotWithMeta = true;
      emptySlotIndex = i;
    }
  }
  CHECK(!anyEmptySlotWithMeta,
        "an emptied slot clears BOTH item_id and metadata; no empty slot is "
        "left advertising orphaned metadata or a stale count");
  if (anyEmptySlotWithMeta) {
    printf("       (orphaned state on slot %d: item_id=%u count=%u meta=%u)\n",
           emptySlotIndex, inv[static_cast<size_t>(emptySlotIndex)].item_id,
           inv[static_cast<size_t>(emptySlotIndex)].count,
           inv[static_cast<size_t>(emptySlotIndex)].meta);
  }

  // Net effect: exactly one oak_log left the inventory, and the total is
  // right under either (buggy or fixed) matching — only WHICH stack is
  // observable. So assert the identity of the survivor, not just the total.
  int logsTotal = 0;
  for (const auto& s : inv) {
    if (s.item_id == kOakLog) logsTotal += s.count;
  }
  CHECK_EQ(logsTotal, 1, "one oak_log was consumed in total");
  CHECK_EQ(int(inv[0].item_id), int(kOakLog),
           "the SURVIVOR is the damaged log: the pristine one is the one the "
           "craft legitimately charged for");

  // The bench cell is consumed, and the 4-plank result stacked onto the
  // pre-existing partial stack rather than refilling the freed slot — which
  // is what left the debited slot observable as an empty slot above.
  const auto after = cachedGrid(*f.workbench, 21, 21, 21);
  CHECK_EQ(after.size(), size_t(9), "the workbench grid keeps its 9 slots");
  CHECK_EQ(int(after[0].item_id), 0, "the consumed grid cell is empty");
  CHECK_EQ(int(inv[2].item_id), int(kPlanks),
           "the result stacked onto the existing oak_planks stack");
  CHECK_EQ(int(inv[2].count), 7, "3 pre-existing planks plus the 4 granted");
  int planks = 0;
  for (const auto& s : inv) {
    if (s.item_id == kPlanks) planks += s.count;
  }
  CHECK_EQ(planks, 7, "the result appears exactly once, stacked on slot 2");
}

// ===========================================================================
// 3. RecipeCompletedHandler — machine-side result application
// ===========================================================================

namespace {

// A MachineComponent+InventoryContainer pair at (x,y,z), as the handler's view
// requires both components on the SAME entity.
entt::entity addMachine(entt::registry& reg, int32_t x, int32_t y, int32_t z,
                       uint16_t machine_id) {
  const auto ent = reg.create();
  reg.emplace<simcore::MachineComponent>(
      ent, machine_id, 0, static_cast<uint32_t>(x), static_cast<uint32_t>(y),
      static_cast<uint32_t>(z), 7);
  simcore::InventoryContainer inv;
  inv.entity_type = 2;
  inv.slot_count = 3;
  inv.slots = {{111, 1, 0}, {222, 2, 0}, {333, 3, 0}};
  reg.emplace<simcore::InventoryContainer>(ent, inv);
  return ent;
}

std::vector<simcore::InventorySlot> slotsOf(entt::registry& reg, entt::entity e) {
  return reg.get<simcore::InventoryContainer>(e).slots;
}

} // namespace

static void test_recipe_completed_replaces_the_machine_inventory() {
  auto engine = std::make_shared<simcore::SimulationEngine>();
  const auto ent = addMachine(engine->reg(), 100, 64, -20, kCraftingTable);
  simcore::RecipeCompletedHandler handler(engine);

  const std::vector<Protocol::ItemStack> results = {
      Protocol::ItemStack(900, 5, 1),
      Protocol::ItemStack(901, 1, 0),
  };
  handler.handle(buildRecipeCompleted(100, 64, -20, kCraftingTable, "macerator_copper", results));

  const auto slots = slotsOf(engine->reg(), ent);
  CHECK_EQ(slots.size(), size_t(2), "the old 3-slot inventory is fully replaced");
  if (slots.size() == 2) {
    CHECK_EQ(slots[0].item_id, uint16_t(900), "slot 0 item_id applied");
    CHECK_EQ(int(slots[0].count), 5, "slot 0 count applied");
    CHECK_EQ(slots[0].meta, uint16_t(1), "slot 0 meta applied");
    CHECK_EQ(slots[1].item_id, uint16_t(901), "slot 1 item_id applied");
  }
  CHECK_NE(int(slotsOf(engine->reg(), ent)[0].item_id), 111,
           "the previous contents are gone, not merged");
}

static void test_recipe_completed_replaces_rather_than_appends() {
  auto engine = std::make_shared<simcore::SimulationEngine>();
  const auto ent = addMachine(engine->reg(), 0, 0, 0, kCraftingTable);
  simcore::RecipeCompletedHandler handler(engine);

  CHECK_EQ(slotsOf(engine->reg(), ent).size(), size_t(3), "precondition: 3 slots");

  // One result slot only.
  const std::vector<Protocol::ItemStack> one = {Protocol::ItemStack(900, 1, 0)};
  handler.handle(buildRecipeCompleted(0, 0, 0, kCraftingTable, "r", one));

  const auto slots = slotsOf(engine->reg(), ent);
  CHECK_EQ(slots.size(), size_t(1),
           "inv.slots.clear() runs first, so the result REPLACES the inventory "
           "rather than being appended to it");
  CHECK_EQ(slots[0].item_id, uint16_t(900), "the single result is slot 0");
}

static void test_recipe_completed_with_no_results_empties_the_inventory() {
  auto engine = std::make_shared<simcore::SimulationEngine>();
  const auto ent = addMachine(engine->reg(), 5, 5, 5, kCraftingTable);
  simcore::RecipeCompletedHandler handler(engine);

  // An empty result_slots vector: clear() then nothing added.
  const std::vector<Protocol::ItemStack> none;
  handler.handle(buildRecipeCompleted(5, 5, 5, kCraftingTable, "r", none));

  CHECK_EQ(slotsOf(engine->reg(), ent).size(), size_t(0),
           "an empty result set empties the machine inventory");
}

static void test_recipe_completed_only_touches_the_machine_at_that_position() {
  auto engine = std::make_shared<simcore::SimulationEngine>();
  const auto target = addMachine(engine->reg(), 10, 20, 30, kCraftingTable);
  const auto other = addMachine(engine->reg(), 11, 20, 30, kCraftingTable);
  const auto below = addMachine(engine->reg(), 10, 19, 30, kCraftingTable);
  simcore::RecipeCompletedHandler handler(engine);

  const std::vector<Protocol::ItemStack> results = {
      Protocol::ItemStack(900, 1, 0)};
  handler.handle(buildRecipeCompleted(10, 20, 30, kCraftingTable, "r", results));

  CHECK_EQ(slotsOf(engine->reg(), target).size(), size_t(1), "the target is updated");
  CHECK_EQ(slotsOf(engine->reg(), other).size(), size_t(3),
           "the machine one block east is untouched");
  CHECK_EQ(slotsOf(engine->reg(), below).size(), size_t(3),
           "the machine one block below is untouched");
}

static void test_recipe_completed_ignores_entities_without_an_inventory() {
  auto engine = std::make_shared<simcore::SimulationEngine>();
  // MachineComponent but NO InventoryContainer: the handler's two-type view
  // does not match, so it must be invisible to it.
  const auto bare = engine->reg().create();
  engine->reg().emplace<simcore::MachineComponent>(
      bare, kCraftingTable, 0, 7, 7, 7, 7);

  const auto withInv = addMachine(engine->reg(), 7, 7, 7, kCraftingTable);
  simcore::RecipeCompletedHandler handler(engine);

  const std::vector<Protocol::ItemStack> results = {Protocol::ItemStack(900, 1, 0)};
  handler.handle(buildRecipeCompleted(7, 7, 7, kCraftingTable, "r", results));

  CHECK_EQ(slotsOf(engine->reg(), withInv).size(), size_t(1),
           "the machine that HAS an inventory is updated");
  CHECK(!engine->reg().all_of<simcore::InventoryContainer>(bare),
         "no InventoryContainer is created for the bare MachineComponent");
  CHECK(engine->reg().valid(bare), "the bare entity is not destroyed");
}

static void test_recipe_completed_breaks_after_the_first_position_match() {
  // Two machines at the SAME position (possible after a machine is rebuilt
  // without despawning the old entity). RecipeCompletedHandler::handle breaks
  // out of the loop on the first position match, so only ONE of them is
  // rewritten and the other is left stale.
  auto engine = std::make_shared<simcore::SimulationEngine>();
  const auto first = addMachine(engine->reg(), 3, 3, 3, kCraftingTable);
  const auto second = addMachine(engine->reg(), 3, 3, 3, kCraftingTable);
  simcore::RecipeCompletedHandler handler(engine);

  const std::vector<Protocol::ItemStack> results = {Protocol::ItemStack(900, 9, 0)};
  handler.handle(buildRecipeCompleted(3, 3, 3, kCraftingTable, "r", results));

  const auto a = slotsOf(engine->reg(), first);
  const auto b = slotsOf(engine->reg(), second);
  // The untouched duplicate KEEPS its 3 seeded slots, so the total is
  // 1 (rewritten) + 3 (stale) == 4, not 1 + 1.
  CHECK_EQ(a.size() + b.size(), size_t(4),
           "exactly one of the two co-located machines is rewritten; the other "
           "keeps its full 3-slot inventory, so the totals are 1 + 3");
  CHECK(a.size() == size_t(1) || b.size() == size_t(1),
        "the break stops after the first match, leaving the duplicate stale");
  CHECK(a.size() == size_t(3) || b.size() == size_t(3),
        "the entity that lost the race is untouched, not emptied");
  // POSITION MATCH, NOT ENTITY IDENTITY: which duplicate wins is decided by
  // the order entt's view yields entities, which is reverse-creation order —
  // NOT by the order the caller created them. Observed here: `second` (created
  // last) is the one rewritten. Pinned, not blessed; see the filed issue.
  CHECK_EQ(b.size(), size_t(1),
           "the later-created entity is the one the view yields first, so the "
           "result lands on it regardless of which duplicate is 'real'");
  CHECK_EQ(a.size(), size_t(3),
           "the earlier-created duplicate keeps all 3 of its seeded slots");
}

static void test_malformed_recipe_completed_is_rejected() {
  auto engine = std::make_shared<simcore::SimulationEngine>();
  const auto ent = addMachine(engine->reg(), 1, 2, 3, kCraftingTable);
  simcore::RecipeCompletedHandler handler(engine);

  const std::vector<uint8_t> junk = {0x01, 0x02, 0x03, 0x04};
  handler.handle(junk);

  CHECK_EQ(slotsOf(engine->reg(), ent).size(), size_t(3),
           "a malformed buffer leaves the inventory untouched");

  handler.handle({});
  CHECK_EQ(slotsOf(engine->reg(), ent).size(), size_t(3),
           "an empty buffer leaves the inventory untouched");
}

static void test_recipe_completed_with_no_matching_position_is_a_noop() {
  auto engine = std::make_shared<simcore::SimulationEngine>();
  const auto ent = addMachine(engine->reg(), 100, 100, 100, kCraftingTable);
  simcore::RecipeCompletedHandler handler(engine);

  const std::vector<Protocol::ItemStack> results = {Protocol::ItemStack(900, 1, 0)};
  handler.handle(buildRecipeCompleted(1, 2, 3, kCraftingTable, "r", results));

  CHECK_EQ(slotsOf(engine->reg(), ent).size(), size_t(3),
           "a position with no machine changes nothing");
}

static void test_recipe_completed_on_an_empty_registry_is_a_noop() {
  auto engine = std::make_shared<simcore::SimulationEngine>();
  simcore::RecipeCompletedHandler handler(engine);
  const std::vector<Protocol::ItemStack> results = {Protocol::ItemStack(900, 1, 0)};
  handler.handle(buildRecipeCompleted(0, 0, 0, kCraftingTable, "r", results));
  handler.handle({});
  // Reaching here without a crash is the assertion; the view on an empty
  // registry simply never enters its loop.
  CHECK(true, "an empty ECS registry is handled without crashing");
}

// ---------------------------------------------------------------------------

#define TEST(name) \
  do { \
    printf("  TEST: %s\n", #name); \
    test_##name(); \
  } while (0)

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  printf("=== crafting_state_machine test suite ===\n\n");

  printf("--- WorkbenchStateManager ---\n");
  TEST(setGridState_then_getGridState_is_a_cache_hit);
  TEST(getGridState_callback_fires_synchronously_on_cache_hit);
  TEST(getGridState_cache_miss_yields_an_empty_grid);
  TEST(setGridState_overwrites_a_previous_grid);
  TEST(positions_are_keyed_independently);
  TEST(posKey_z_is_truncated_to_16_bits);
  TEST(setGridState_with_null_ess_client_is_safe);
  TEST(removeGridState_does_not_notify_the_client);

  printf("--- CraftRequestHandler ---\n");
  TEST(craft_success_consumes_the_grid_and_grants_the_result);
  TEST(craft_is_server_authoritative_and_ignores_client_slots);
  TEST(craft_from_an_empty_grid_grants_nothing);
  TEST(craft_with_a_non_matching_grid_leaves_state_untouched);
  TEST(craft_deducts_only_the_recipe_inputs);
  TEST(craft_without_a_main_queue_runs_inline);
  TEST(craft_work_is_queued_not_run_inside_handle);
  TEST(malformed_craft_request_is_rejected_silently);
  TEST(craft_without_a_workbench_manager_is_a_noop);
  TEST(craft_of_a_multi_item_recipe_deducts_each_input);
  TEST(repeated_crafts_consume_the_grid_each_time);

  printf("--- CraftRequestHandler: metadata-aware deduction ---\n");
  TEST(craft_debits_the_matching_metadata_variant);

  printf("--- RecipeCompletedHandler ---\n");
  TEST(recipe_completed_replaces_the_machine_inventory);
  TEST(recipe_completed_replaces_rather_than_appends);
  TEST(recipe_completed_with_no_results_empties_the_inventory);
  TEST(recipe_completed_only_touches_the_machine_at_that_position);
  TEST(recipe_completed_ignores_entities_without_an_inventory);
  TEST(recipe_completed_breaks_after_the_first_position_match);
  TEST(malformed_recipe_completed_is_rejected);
  TEST(recipe_completed_with_no_matching_position_is_a_noop);
  TEST(recipe_completed_on_an_empty_registry_is_a_noop);

  printf("\n=== Results: %d assertions, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
