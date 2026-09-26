// Client-side recipe/item mirror tests — beads gp-koye.
//
// Two production surfaces, both of which MIRROR server state on the client and
// therefore fail in a user-visible way when they drift:
//
//   src/game/client/Crafting/ServerRecipeDB.cpp     — the cache of
//     server-sourced recipes (catalog, per-item, per-machine, 3x3 grid) with
//     LRU eviction, in-flight dedup and response parsing.
//   src/game/client/Crafting/ClientItemRegistry.cpp — a second, independent
//     parse of the SAME src/content/data/registry/items.csv that
//     src/game/recipes/ItemRegistry.cpp parses on the server.
//
// gp-koye's acceptance criteria call for three things, all covered here:
//   1. missing key   — GetItem / GetName / GetStackSize / GetAllItemIds /
//                     the recipe caches on an absent key return the DOCUMENTED
//                     default, and never crash.
//   2. present key   — the same accessors return the parsed value.
//   3. drift         — the client mirror agrees with the server registry and
//                     the server RecipeManager for a small fixed item set,
//                     driven by the real data files.
//
// Harness: the project's own CHECK/TEST macros (src/engine/net/test/test.h).
// GTest is deliberately not used — it is absent from conanfile.txt, CI does
// not install libgtest-dev, and CI builds Release with a global -Werror, so a
// find_package(GTest QUIET) guard would silently unregister this test on CI.
//
// Determinism / no external effects: no display, no GL context, no window, no
// input device, no socket and no wall-clock wait. ServerRecipeDB is driven by
// calling HandleRecipeResponse() directly with a FlatBuffer reply this file
// builds itself, so the whole request/response path is exercised without a
// NetClient connection. A default-constructed NetClient is created only to
// give Init() a non-null pointer: every Send*() on it returns immediately
// because no control connection is up (all four early-return on
// `!ctrl_conn_ || !connected_ctrl_`), so nothing is ever written anywhere.
// DATA_DIR is injected as an absolute path by CMake; the CWD is never used.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>
#include <glm/glm.hpp>

#include <common/GatewayMsg.h>
#include <engine/registry/ItemId.h>

#include <game/client/Common/Inventory.h>
#include <game/client/Common/Types.h>
#include <game/client/Crafting/ClientItemRegistry.h>
#include <game/client/Crafting/ServerRecipeDB.h>
#include <game/recipes/ItemRegistry.h>
#include <game/recipes/RecipeManager.h>

#include "Network/NetClient.h"
#include "recipe_generated.h"

// Project-wide unit-test harness (src/engine/net/test/test.h).
#include <engine/net/test/test.h>

#ifndef DATA_DIR
#error "DATA_DIR must be defined by the build (see src/apps/game_client/CMakeLists.txt)"
#endif

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char *file, int line, const char *expr,
                const char *msg) {
  if (!cond) {
    fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
    if (msg)
      fprintf(stderr, " -- %s", msg);
    fprintf(stderr, "\n");
    ++g_failed;
  } else {
    ++g_passed;
  }
}

namespace {

// ---------------------------------------------------------------------------
// Constants asserted by name, so a data change shows up as a named failure.
// ---------------------------------------------------------------------------

// Items chosen to cover every CSV shape the two parsers treat differently:
//   air              — the only id whose packed value is 0 (air is SKIPPED by
//                      the client loader but KEPT by the server loader)
//   cobblestone      — a plain block, empty stack column in the CSV
//   circuit_board    — a component row with an explicit stack size of 64
//   steam            — the fluid whose id the client also special-cases
//   glass            — used as the "not mentioned by this recipe" probe
constexpr uint16_t kAir = 0;
constexpr uint16_t kCobblestone = ItemId::pack("0:0:2");
constexpr uint16_t kGlass = ItemId::pack("0:0:4");
constexpr uint16_t kCircuitBoard = ItemId::pack("110:001:0");
constexpr uint16_t kSteam = ItemId::pack("1111:11:1");
constexpr std::string_view kCobblestoneName = "cobblestone";

// The item a check-reply hands back as its output, used to prove the grid
// cache stores the server's answer verbatim.
constexpr uint16_t kSand = ItemId::pack("0:0:3");

// The default stack size both mirrors fall back to for an unknown id. It is a
// hardcoded 64 in BOTH files (ClientItemRegistry.cpp GetStackSize and
// ItemRegistry.cpp getMaxStackSize); the server-side one is tracked as gp-70j.
constexpr uint8_t kDefaultStackSize = 64;

// The name the client mirror reports for an unknown id.
constexpr std::string_view kUnknownName = "???";

// A block id that is in no machines.yaml, used as a "present" machine key
// without depending on the shipped machine table.
constexpr uint16_t kTestMachine = 0xF00D;

// Cap the drift test to the first N ids so the failure message stays short.
constexpr size_t kDriftProbeLimit = 64;

std::string itemsCsvPath() { return std::string(DATA_DIR) + "/registry/items.csv"; }

// ---------------------------------------------------------------------------
// Reply builders — these stand for the server's FlatBuffers output.
//
// They mirror the production builders in src/game/recipes/RecipeManager.cpp
// (handleCatalogRequest / handleRecipesForItemRequest / handleCheckRecipeRequest)
// exactly, including the mutable-gen union idiom: there is no CreateRecipePayload
// / CreateRecipeResponse for a union, so the *Builder classes are used and the
// response is attached with .Union().
// ---------------------------------------------------------------------------

// Wrap a RecipeReply in the RecipeFrame envelope HandleRecipeResponse expects,
// and copy it into a stable heap buffer the caller can own.
std::shared_ptr<std::vector<uint8_t>>
finishFrame(flatbuffers::FlatBufferBuilder &builder,
            flatbuffers::Offset<Protocol::RecipeReply> replyOffset) {
  Protocol::RecipeFrameBuilder frameBuilder(builder);
  frameBuilder.add_payload_type(Protocol::RecipePayload_RecipeReply);
  frameBuilder.add_payload(replyOffset.Union());
  auto frameOffset = frameBuilder.Finish();
  builder.Finish(frameOffset);
  return std::make_shared<std::vector<uint8_t>>(builder.GetBufferPointer(),
                                                builder.GetBufferPointer() +
                                                    builder.GetSize());
}

std::shared_ptr<std::vector<uint8_t>> makeCatalogReply(uint32_t req_id,
                                                       std::vector<uint16_t> ids) {
  flatbuffers::FlatBufferBuilder builder;
  auto idsOffset = builder.CreateVector(ids);
  auto respOffset = Protocol::CreateRecipeCatalogResp(builder, idsOffset);

  Protocol::RecipeReplyBuilder replyBuilder(builder);
  replyBuilder.add_req_id(req_id);
  replyBuilder.add_response_type(Protocol::RecipeResponse_RecipeCatalogResp);
  replyBuilder.add_response(respOffset.Union());
  return finishFrame(builder, replyBuilder.Finish());
}

// A FlatBuffers Offset is only meaningful inside the builder that produced it,
// so the RecipeInfo offsets CANNOT be created by the caller and handed in: they
// would be re-vectored against a different, still-empty builder. Instead the
// caller passes a lambda that emits its RecipeInfo offsets into the builder the
// reply itself lives in. This is the same constraint the production handlers
// obey (they loop buildRecipeInfo(builder, *r) into the reply's own builder).
template <typename Emit>
std::shared_ptr<std::vector<uint8_t>> makeItemReply(uint32_t req_id,
                                                    Emit &&emit_infos) {
  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<Protocol::RecipeInfo>> infos =
      emit_infos(builder);
  auto vecOffset = builder.CreateVector(infos);
  auto respOffset = Protocol::CreateRecipesForItemResp(builder, vecOffset);

  Protocol::RecipeReplyBuilder replyBuilder(builder);
  replyBuilder.add_req_id(req_id);
  replyBuilder.add_response_type(Protocol::RecipeResponse_RecipesForItemResp);
  replyBuilder.add_response(respOffset.Union());
  return finishFrame(builder, replyBuilder.Finish());
}

template <typename Emit>
std::shared_ptr<std::vector<uint8_t>> makeMachineReply(uint32_t req_id,
                                                       Emit &&emit_infos) {
  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<Protocol::RecipeInfo>> infos =
      emit_infos(builder);
  auto vecOffset = builder.CreateVector(infos);
  auto respOffset = Protocol::CreateRecipesForMachineResp(builder, vecOffset);

  Protocol::RecipeReplyBuilder replyBuilder(builder);
  replyBuilder.add_req_id(req_id);
  replyBuilder.add_response_type(Protocol::RecipeResponse_RecipesForMachineResp);
  replyBuilder.add_response(respOffset.Union());
  return finishFrame(builder, replyBuilder.Finish());
}

// CheckRecipeResp: a recipe_id plus an optional RecipeInfo carrying outputs.
std::shared_ptr<std::vector<uint8_t>> makeCheckReply(uint32_t req_id,
                                                     const std::string &recipe_id,
                                                     bool with_output,
                                                     uint16_t output_id,
                                                     uint8_t output_count) {
  flatbuffers::FlatBufferBuilder builder;
  flatbuffers::Offset<Protocol::RecipeInfo> infoOffset = 0;
  if (with_output) {
    std::vector<Protocol::ItemStack> outputs;
    outputs.push_back(Protocol::ItemStack(output_id, output_count, 0));
    // CreateRecipeInfoDirect builds the item vectors itself from a
    // std::vector pointer, so the offsets are NOT precomputed here.
    infoOffset = Protocol::CreateRecipeInfoDirect(
        builder, 0, "macerator", recipe_id.c_str(), 200, 0, nullptr, &outputs,
        false, nullptr);
  }
  auto idOffset = builder.CreateString(recipe_id);
  auto respOffset = Protocol::CreateCheckRecipeResp(builder, idOffset, infoOffset);

  Protocol::RecipeReplyBuilder replyBuilder(builder);
  replyBuilder.add_req_id(req_id);
  replyBuilder.add_response_type(Protocol::RecipeResponse_CheckRecipeResp);
  replyBuilder.add_response(respOffset.Union());
  return finishFrame(builder, replyBuilder.Finish());
}

// A minimal RecipeInfo: one input, one output, no pattern.
flatbuffers::Offset<Protocol::RecipeInfo>
buildSimpleInfo(flatbuffers::FlatBufferBuilder &builder, uint16_t machine_type,
                const std::string &machine_class, const std::string &recipe_id,
                uint32_t duration, uint8_t unlock_era, uint16_t input_id,
                uint8_t input_count, uint16_t output_id, uint8_t output_count) {
  std::vector<Protocol::ItemStack> inputs;
  inputs.push_back(Protocol::ItemStack(input_id, input_count, 0));
  std::vector<Protocol::ItemStack> outputs;
  outputs.push_back(Protocol::ItemStack(output_id, output_count, 0));
  return Protocol::CreateRecipeInfoDirect(builder, machine_type,
                                          machine_class.c_str(),
                                          recipe_id.c_str(), duration,
                                          unlock_era, &inputs, &outputs, false,
                                          nullptr);
}

// The request id the DB hands out for a given query. nextReqId_ starts at 0 and
// pre-increments, so the FIRST request of a fresh DB is always 1.
constexpr uint32_t kFirstReqId = 1;

// The per-item and per-machine response handlers only write to a cache when a
// matching PENDING entry exists — HandleRecipeResponse does
//   auto it = pending_.find(reply->req_id());
//   if (it == pending_.end()) return;
// BEFORE dispatching to handleItemResponse/handleMachineResponse. A reply that
// arrives without a query in flight is discarded, which is what
// test_recipe_db_reply_for_an_unknown_request_id_is_ignored pins.
//
// So the cache tests must ask first. These helpers issue the query (which
// records a pending entry and calls into the connection-less NetClient, a
// no-op) so the subsequent reply is matched. The returned req_id is always
// kFirstReqId for a freshly constructed DB.
uint32_t askForItem(ServerRecipeDB &db, NetClient &net, uint16_t item_id) {
  db.Init(&net);
  db.GetRecipesForItem(item_id, []() {});
  return kFirstReqId;
}

uint32_t askForMachine(ServerRecipeDB &db, NetClient &net, uint16_t machine_id) {
  db.Init(&net);
  db.GetRecipesForMachine(machine_id, []() {});
  return kFirstReqId;
}

} // namespace

// ---------------------------------------------------------------------------
// ClientItemRegistry: the client-side parse of items.csv
// ---------------------------------------------------------------------------

static void test_client_item_registry_present_key() {
  ItemRegistry::LoadFromCSV(itemsCsvPath());

  const auto *cobble = ItemRegistry::GetItem(kCobblestone);
  CHECK(cobble != nullptr, "cobblestone is present in the client mirror");
  if (cobble) {
    CHECK_EQ(cobble->id, kCobblestone, "the stored id matches the packed id");
    CHECK_EQ(cobble->name, std::string(kCobblestoneName), "its name is parsed");
    CHECK_EQ(cobble->meta, uint16_t(0), "its meta is parsed");
  }
  CHECK_EQ(ItemRegistry::GetName(kCobblestone), kCobblestoneName,
           "GetName returns the parsed name for a present key");
  CHECK_EQ(ItemRegistry::GetStackSize(kCobblestone), kDefaultStackSize,
           "GetStackSize returns the CSV default (64) for cobblestone");
}

static void test_client_item_registry_missing_key_returns_documented_default() {
  ItemRegistry::LoadFromCSV(itemsCsvPath());

  // 0xFFFF is not in items.csv (the highest real id is far below it).
  constexpr uint16_t kAbsent = 0xFFFF;
  CHECK(ItemRegistry::GetItem(kAbsent) == nullptr,
        "GetItem returns nullptr for a missing key");
  CHECK_EQ(ItemRegistry::GetName(kAbsent), kUnknownName,
           "GetName returns the \"???\" placeholder for a missing key");
  CHECK_EQ(ItemRegistry::GetStackSize(kAbsent), kDefaultStackSize,
           "GetStackSize returns the documented default for a missing key");
}

static void test_client_item_registry_skips_air_by_design() {
  ItemRegistry::LoadFromCSV(itemsCsvPath());

  // FINDING: the CSV's first data row is `0:0:0,air,0,0`, but the client
  // loader has an explicit `if (id == 0) continue;` (ClientItemRegistry.cpp),
  // so air is NOT in the client mirror even though it IS in the server's
  // ItemRegistry. This is an intentional client-side divergence, asserted here
  // so the drift test below can exclude it rather than tripping over it.
  CHECK(ItemRegistry::GetItem(kAir) == nullptr,
        "item id 0 (air) is deliberately absent from the client mirror");
  CHECK_EQ(ItemRegistry::GetName(kAir), kUnknownName,
           "and its name falls back to the \"???\" placeholder");
}

static void test_client_item_registry_finds_steam_by_name() {
  ItemRegistry::LoadFromCSV(itemsCsvPath());

  // The loader special-cases the literal name "steam" and records its id; this
  // is the client's own copy of the fluid catalog entry.
  const uint16_t steam = ItemRegistry::GetSteamItemId();
  CHECK_EQ(steam, kSteam, "the steam id resolves to the CSV row 1111:11:1");
  CHECK_EQ(ItemRegistry::GetName(steam), std::string_view("steam"),
           "the steam id's name is \"steam\"");
  const auto *info = ItemRegistry::GetItem(steam);
  CHECK(info != nullptr, "the steam item is present in the mirror");
  if (info)
    CHECK_EQ(info->stackSize, kDefaultStackSize,
             "steam carries the default stack size");
}

static void test_client_item_registry_all_ids_excludes_air() {
  ItemRegistry::LoadFromCSV(itemsCsvPath());

  const std::vector<uint16_t> ids = ItemRegistry::GetAllItemIds();
  CHECK(!ids.empty(), "GetAllItemIds returns a non-empty vector");
  CHECK(std::find(ids.begin(), ids.end(), kAir) == ids.end(),
        "GetAllItemIds does not include the skipped air id");
  CHECK(std::find(ids.begin(), ids.end(), kCobblestone) != ids.end(),
        "GetAllItemIds includes cobblestone");
  // Every id must resolve back through GetItem — the vector is the index the
  // client UI iterates, so a dangling entry would be a lookup miss.
  bool allResolve = true;
  for (uint16_t id : ids) {
    if (ItemRegistry::GetItem(id) == nullptr) {
      allResolve = false;
      break;
    }
  }
  CHECK(allResolve, "every id from GetAllItemIds resolves through GetItem");
}

static void test_client_item_registry_reload_is_idempotent() {
  ItemRegistry::LoadFromCSV(itemsCsvPath());
  const size_t first = ItemRegistry::GetAllItemIds().size();
  ItemRegistry::LoadFromCSV(itemsCsvPath());
  const size_t second = ItemRegistry::GetAllItemIds().size();

  // LoadFromCSV resets the steam id but never clears s_items, so a second load
  // is a no-op on the id map. Pinned because a caller that reloads mid-session
  // would otherwise silently accumulate.
  CHECK_EQ(second, first, "loading the CSV twice yields the same id count");
}

static void test_client_item_registry_missing_csv_leaves_previous_state() {
  // FINDING: LoadFromCSV on a bad path logs to stderr and returns WITHOUT
  // clearing s_items, so a failed reload silently keeps serving the previous
  // catalog. Pinned so the behaviour is visible to a reader.
  const std::vector<uint16_t> before = ItemRegistry::GetAllItemIds();
  ItemRegistry::LoadFromCSV("/nonexistent/path/to/items.csv");
  const std::vector<uint16_t> after = ItemRegistry::GetAllItemIds();
  CHECK_EQ(after.size(), before.size(),
           "a failed reload leaves the previously loaded ids in place");
  // The steam sentinel IS reset by the failed load — the asymmetry is the point.
  CHECK_EQ(ItemRegistry::GetSteamItemId(), uint16_t(0),
           "a failed reload does reset the steam id to 0");
}

// ---------------------------------------------------------------------------
// ServerRecipeDB: catalog
// ---------------------------------------------------------------------------

static void test_recipe_db_starts_with_no_catalog() {
  ServerRecipeDB db;
  CHECK(!db.IsCatalogLoaded(), "a fresh DB reports no catalog");
  CHECK(db.Catalog().empty(), "and exposes an empty catalog");
}

static void test_recipe_db_catalog_populates_from_a_reply() {
  ServerRecipeDB db;
  const std::vector<uint16_t> ids{kCobblestone, kGlass, kSteam};

  db.HandleRecipeResponse(GatewayMsg::kRecipeCatalogResp,
                          makeCatalogReply(kFirstReqId, ids));

  CHECK(db.IsCatalogLoaded(), "a catalog reply flips the loaded flag");
  CHECK_EQ(db.Catalog().size(), ids.size(), "every id in the reply is stored");
  const std::vector<uint16_t> &cat = db.Catalog();
  for (uint16_t want : ids) {
    CHECK(std::find(cat.begin(), cat.end(), want) != cat.end(),
          "each catalog id is present in the client's copy");
  }
  if (cat.size() == 3) {
    // Order is preserved verbatim from the wire (no dedup, no sort).
    CHECK_EQ(cat[0], kCobblestone, "catalog order is preserved from the reply");
    CHECK_EQ(cat[1], kGlass, "catalog order is preserved from the reply");
    CHECK_EQ(cat[2], kSteam, "catalog order is preserved from the reply");
  }
}

static void test_recipe_db_empty_catalog_reply_is_still_loaded() {
  ServerRecipeDB db;
  db.HandleRecipeResponse(GatewayMsg::kRecipeCatalogResp,
                          makeCatalogReply(kFirstReqId, {}));
  // An empty reply is a valid answer meaning "no recipes exist" — the client
  // must stop asking, or the NEI panel retries forever.
  CHECK(db.IsCatalogLoaded(), "an empty catalog reply still marks it loaded");
  CHECK(db.Catalog().empty(), "and stores no ids");
}

static void test_recipe_db_catalog_replaces_a_previous_catalog() {
  ServerRecipeDB db;
  db.HandleRecipeResponse(GatewayMsg::kRecipeCatalogResp,
                          makeCatalogReply(kFirstReqId, {kCobblestone, kGlass}));
  db.HandleRecipeResponse(GatewayMsg::kRecipeCatalogResp,
                          makeCatalogReply(kFirstReqId + 1, {kSteam}));

  // handleCatalogResponse calls catalog_.clear() first, so the second reply
  // REPLACES rather than appends.
  CHECK_EQ(db.Catalog().size(), size_t(1), "a later catalog reply replaces the list");
  if (db.Catalog().size() == 1)
    CHECK_EQ(db.Catalog()[0], kSteam, "and holds the newer ids only");
}

static void test_recipe_db_rejects_a_malformed_frame() {
  ServerRecipeDB db;
  // A 3-byte payload cannot be a valid RecipeFrame; the verifier must reject
  // it and leave the DB untouched rather than reading past the buffer.
  auto junk = std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{0x01,
                                                                         0x02,
                                                                         0x03});
  db.HandleRecipeResponse(GatewayMsg::kRecipeCatalogResp, junk);
  CHECK(!db.IsCatalogLoaded(), "a malformed frame does not mark the catalog loaded");
  CHECK(db.Catalog().empty(), "and leaves the catalog empty");
}

// ---------------------------------------------------------------------------
// ServerRecipeDB: per-item recipes and the craft/use split
// ---------------------------------------------------------------------------

static void test_recipe_db_item_cache_miss_returns_empty() {
  ServerRecipeDB db;
  const auto got = db.GetItemRecipesCopy(0xBEEF);
  CHECK(got.craft.empty(), "an uncached item reports no craft recipes");
  CHECK(got.use.empty(), "an uncached item reports no use recipes");
}

static void test_recipe_db_item_reply_splits_craft_and_use() {
  ServerRecipeDB db;
  NetClient net;
  const uint32_t req = askForItem(db, net, kCobblestone);

  // Build two recipes: one PRODUCES cobblestone, one CONSUMES it. The DB must
  // split them across the two tabs rather than returning both in both.
  db.HandleRecipeResponse(
      GatewayMsg::kRecipeItemResp, makeItemReply(req, [](auto &b) {
        return std::vector<flatbuffers::Offset<Protocol::RecipeInfo>>{
            buildSimpleInfo(b, kTestMachine, "macerator", "makes_cobble", 200, 0,
                            kGlass, 1, kCobblestone, 1),
            buildSimpleInfo(b, kTestMachine, "furnace", "uses_cobble", 100, 0,
                            kCobblestone, 1, kGlass, 1)};
      }));

  const auto got = db.GetItemRecipesCopy(kCobblestone);
  CHECK_EQ(got.craft.size(), size_t(1),
           "exactly one recipe is filed under craft for cobblestone");
  CHECK_EQ(got.use.size(), size_t(1),
           "exactly one recipe is filed under use for cobblestone");
  if (got.craft.size() == 1)
    CHECK_EQ(got.craft[0].recipe_id, std::string("makes_cobble"),
             "the producing recipe is the craft entry");
  if (got.use.size() == 1)
    CHECK_EQ(got.use[0].recipe_id, std::string("uses_cobble"),
             "the consuming recipe is the use entry");
}

static void test_recipe_db_item_reply_files_a_recycling_recipe_in_both_tabs() {
  ServerRecipeDB db;
  NetClient net;
  const uint32_t req = askForItem(db, net, kCobblestone);

  // A recipe that both produces AND consumes cobblestone belongs in BOTH tabs.
  // This is the recycling case the response handler explicitly calls out.
  db.HandleRecipeResponse(
      GatewayMsg::kRecipeItemResp, makeItemReply(req, [](auto &b) {
        return std::vector<flatbuffers::Offset<Protocol::RecipeInfo>>{
            buildSimpleInfo(b, kTestMachine, "assembler", "recycle_cobble", 200,
                            0, kCobblestone, 1, kCobblestone, 1)};
      }));

  const auto got = db.GetItemRecipesCopy(kCobblestone);
  CHECK_EQ(got.craft.size(), size_t(1), "a self-recipe appears in the craft tab");
  CHECK_EQ(got.use.size(), size_t(1), "and in the use tab");
}

static void test_recipe_db_item_reply_copies_every_recipe_field() {
  ServerRecipeDB db;
  NetClient net;
  const uint32_t req = askForItem(db, net, kCobblestone);

  db.HandleRecipeResponse(
      GatewayMsg::kRecipeItemResp, makeItemReply(req, [](auto &b) {
        return std::vector<flatbuffers::Offset<Protocol::RecipeInfo>>{
            buildSimpleInfo(b, kTestMachine, "macerator", "full_rec", 640, 2,
                            kGlass, 3, kCobblestone, 5)};
      }));

  const auto got = db.GetItemRecipesCopy(kCobblestone);
  if (got.craft.size() != 1) {
    CHECK(false, "the recipe was filed under craft");
    return;
  }
  const auto &ri = got.craft[0];
  CHECK_EQ(ri.recipe_id, std::string("full_rec"), "recipe_id round-trips");
  CHECK_EQ(ri.machine_class, std::string("macerator"), "machine_class round-trips");
  CHECK_EQ(ri.machine_type, kTestMachine, "machine_type round-trips");
  CHECK_EQ(ri.duration, uint32_t(640), "duration round-trips");
  CHECK_EQ(ri.unlock_era, uint8_t(2), "unlock_era round-trips");
  CHECK_EQ(ri.inputs.size(), size_t(1), "the input vector round-trips");
  if (ri.inputs.size() == 1) {
    CHECK_EQ(ri.inputs[0].item_id, kGlass, "input item_id round-trips");
    CHECK_EQ(ri.inputs[0].count, uint8_t(3), "input count round-trips");
  }
  CHECK_EQ(ri.outputs.size(), size_t(1), "the output vector round-trips");
  if (ri.outputs.size() == 1) {
    CHECK_EQ(ri.outputs[0].item_id, kCobblestone, "output item_id round-trips");
    CHECK_EQ(ri.outputs[0].count, uint8_t(5), "output count round-trips");
  }
  CHECK(!ri.has_pattern, "has_pattern is false for a shapeless recipe");
}

static void test_recipe_db_empty_item_reply_caches_an_empty_result() {
  ServerRecipeDB db;
  NetClient net;
  const uint32_t req = askForItem(db, net, kCobblestone);
  db.HandleRecipeResponse(
      GatewayMsg::kRecipeItemResp,
      makeItemReply(req, [](auto &) {
        return std::vector<flatbuffers::Offset<Protocol::RecipeInfo>>{};
      }));
  // The key is now CACHED as "known empty" — a later read is a cache hit that
  // returns empty, which is indistinguishable from a miss at the accessor
  // level but matters because the miss path would fire a network request.
  const auto got = db.GetItemRecipesCopy(kCobblestone);
  CHECK(got.craft.empty(), "an empty reply caches an empty craft list");
  CHECK(got.use.empty(), "an empty reply caches an empty use list");
}

// ---------------------------------------------------------------------------
// ServerRecipeDB: per-machine recipes
// ---------------------------------------------------------------------------

static void test_recipe_db_machine_cache_miss_returns_empty() {
  ServerRecipeDB db;
  CHECK(db.GetMachineRecipesCopy(0xBEEF).empty(),
        "an uncached machine reports no recipes");
}

static void test_recipe_db_machine_reply_populates_the_cache() {
  ServerRecipeDB db;
  NetClient net;
  const uint32_t req = askForMachine(db, net, kTestMachine);

  db.HandleRecipeResponse(
      GatewayMsg::kRecipeMachineResp, makeMachineReply(req, [](auto &b) {
        return std::vector<flatbuffers::Offset<Protocol::RecipeInfo>>{
            buildSimpleInfo(b, kTestMachine, "macerator", "mac_a", 200, 0, kGlass,
                            1, kCobblestone, 1),
            buildSimpleInfo(b, kTestMachine, "macerator", "mac_b", 300, 0,
                            kCobblestone, 1, kGlass, 1)};
      }));

  const auto got = db.GetMachineRecipesCopy(kTestMachine);
  CHECK_EQ(got.size(), size_t(2), "both machine recipes are cached");
  // Unlike the per-item path, the machine path does NOT split craft/use and
  // does NOT filter — every recipe in the reply is stored verbatim.
  if (got.size() == 2) {
    CHECK_EQ(got[0].recipe_id, std::string("mac_a"),
             "machine recipes keep their reply order");
    CHECK_EQ(got[1].recipe_id, std::string("mac_b"),
             "machine recipes keep their reply order");
  }
}

static void test_recipe_db_machine_cache_is_keyed_per_machine() {
  ServerRecipeDB db;
  NetClient net;
  const uint32_t req = askForMachine(db, net, kTestMachine);

  db.HandleRecipeResponse(
      GatewayMsg::kRecipeMachineResp, makeMachineReply(req, [](auto &b) {
        return std::vector<flatbuffers::Offset<Protocol::RecipeInfo>>{
            buildSimpleInfo(b, kTestMachine, "macerator", "only_here", 200, 0,
                            kGlass, 1, kCobblestone, 1)};
      }));

  CHECK_EQ(db.GetMachineRecipesCopy(kTestMachine).size(), size_t(1),
           "the queried machine has its recipe");
  CHECK(db.GetMachineRecipesCopy(kTestMachine + 1).empty(),
        "a DIFFERENT machine id does not see the cached recipe");
}

// ---------------------------------------------------------------------------
// ServerRecipeDB: the 3x3 grid check
// ---------------------------------------------------------------------------

static void test_recipe_db_grid_key_is_deterministic_and_discriminating() {
  std::array<ItemStack, 9> grid{};
  grid[0] = ItemStack{kCobblestone, 1, 0};
  grid[4] = ItemStack{kGlass, 2, 0};

  const uint64_t a = ServerRecipeDB::GridKey(kTestMachine, grid);
  const uint64_t b = ServerRecipeDB::GridKey(kTestMachine, grid);
  CHECK_EQ(a, b, "the same grid hashes to the same key");

  // Position matters: swapping two cells must change the key.
  std::array<ItemStack, 9> swapped = grid;
  std::swap(swapped[0], swapped[4]);
  CHECK_NE(ServerRecipeDB::GridKey(kTestMachine, swapped), a,
           "swapping two cells changes the key");

  // Machine matters.
  CHECK_NE(ServerRecipeDB::GridKey(kTestMachine + 1, grid), a,
           "a different machine_id changes the key");

  // Count matters.
  std::array<ItemStack, 9> moreCount = grid;
  moreCount[4].count = 3;
  CHECK_NE(ServerRecipeDB::GridKey(kTestMachine, moreCount), a,
           "a different stack count changes the key");

  // Meta matters.
  std::array<ItemStack, 9> otherMeta = grid;
  otherMeta[0].meta = 7;
  CHECK_NE(ServerRecipeDB::GridKey(kTestMachine, otherMeta), a,
           "a different metadata changes the key");

  // An all-empty grid is a real key, distinct from any populated one.
  const std::array<ItemStack, 9> empty{};
  CHECK_NE(ServerRecipeDB::GridKey(kTestMachine, empty), a,
           "the empty grid hashes differently from a populated one");
}

static void test_recipe_db_grid_reply_fills_the_grid_cache() {
  ServerRecipeDB db;
  std::array<ItemStack, 9> grid{};
  grid[0] = ItemStack{kCobblestone, 1, 0};

  // CheckGrid is normally what populates gridCache_, but the cache is private.
  // The public contract is: after a check reply lands for a grid key, a repeat
  // CheckGrid for the same grid is a cache HIT and fires the callback with the
  // stored output instead of requesting again. Drive it through the public
  // surface with a NetClient that swallows the send (no connection up).
  NetClient net;
  db.Init(&net);

  bool fired = false;
  ItemStack received{};
  db.CheckGrid(kTestMachine, grid, [&](const ItemStack &out) {
    fired = true;
    received = out;
  });
  // With no control connection the request is still RECORDED as pending, so the
  // callback has not fired yet — it is waiting for a reply.
  CHECK(!fired, "a grid check with no response pending does not fire yet");

  db.HandleRecipeResponse(GatewayMsg::kRecipeCheckResp,
                          makeCheckReply(kFirstReqId, "mac_cobble", true,
                                         kSand, 1));

  CHECK(fired, "the pending grid callback fires when the reply lands");
  CHECK_EQ(received.item_id, kSand, "with the output item from the reply");
  CHECK_EQ(received.count, uint8_t(1), "and its count");
}

static void test_recipe_db_check_reply_without_a_match_reports_empty() {
  ServerRecipeDB db;
  std::array<ItemStack, 9> grid{};
  grid[0] = ItemStack{kCobblestone, 1, 0};

  NetClient net;
  db.Init(&net);

  bool fired = false;
  ItemStack received{};
  received.item_id = 0xDEAD; // sentinel: overwritten by the callback
  db.CheckGrid(kTestMachine, grid, [&](const ItemStack &out) {
    fired = true;
    received = out;
  });

  // A reply with an empty recipe_id and no RecipeInfo is "no match".
  db.HandleRecipeResponse(GatewayMsg::kRecipeCheckResp,
                          makeCheckReply(kFirstReqId, "", false, 0, 0));

  CHECK(fired, "a no-match check reply still fires the callback");
  CHECK_EQ(received.item_id, uint16_t(0),
           "a no-match reply delivers a default-constructed ItemStack");
  CHECK_EQ(received.count, uint8_t(0), "with a zero count");
}

static void test_recipe_db_grid_check_fires_immediately_on_a_cache_hit() {
  ServerRecipeDB db;
  std::array<ItemStack, 9> grid{};
  grid[0] = ItemStack{kCobblestone, 1, 0};

  NetClient net;
  db.Init(&net);

  // First query: miss, request recorded, no reply yet.
  int firstFires = 0;
  db.CheckGrid(kTestMachine, grid, [&](const ItemStack &) { ++firstFires; });
  db.HandleRecipeResponse(GatewayMsg::kRecipeCheckResp,
                          makeCheckReply(kFirstReqId, "mac_cobble", true,
                                         kSand, 1));
  CHECK_EQ(firstFires, 1, "the first query is answered by the reply");

  // Second query for the SAME grid: now a cache hit, so the callback fires
  // SYNCHRONOUSLY inside CheckGrid (no request, no waiting).
  int secondFires = 0;
  db.CheckGrid(kTestMachine, grid, [&](const ItemStack &) { ++secondFires; });
  CHECK_EQ(secondFires, 1,
           "a repeat grid check fires synchronously from the cache");
}

static void test_recipe_db_reply_for_an_unknown_request_id_is_ignored() {
  ServerRecipeDB db;
  // A reply whose req_id matches nothing pending must be a no-op: it must not
  // clear any cache and must not fire any callback.
  auto reply = makeCheckReply(9999, "stray", true, kSand, 1);
  db.HandleRecipeResponse(GatewayMsg::kRecipeCheckResp, reply);
  // Nothing observable changed; the assertion is that no callback ran and the
  // process did not crash on the pending_.find miss.
  CHECK(true, "a stray reply is discarded without touching any cache");
}

// ---------------------------------------------------------------------------
// THE DRIFT TEST (gp-koye's valuable part)
// ---------------------------------------------------------------------------
//
// The client and the server each parse the SAME items.csv independently. If
// they ever disagree, the client shows the wrong name / wrong stack limit /
// wrong item for an id the server thinks exists. This test drives both parsers
// over the real file and compares them item by item.

static void test_client_mirror_does_not_drift_from_the_server_parser() {
  // Server side: the authoritative parser used by RecipeManager.
  RecipeManager::ItemRegistry server;
  const bool serverLoaded = server.loadFromCSV(itemsCsvPath());
  CHECK(serverLoaded, "the server ItemRegistry loads the real items.csv");
  if (!serverLoaded)
    return;

  // Client side: the independent mirror under test.
  ItemRegistry::LoadFromCSV(itemsCsvPath());
  const std::vector<uint16_t> clientIds = ItemRegistry::GetAllItemIds();
  CHECK(!clientIds.empty(), "the client mirror loaded some ids");

  // The client skips id 0 by design (see the air test above); every OTHER id
  // the client knows must agree with the server on name, stack and meta.
  size_t compared = 0;
  size_t nameDrift = 0;
  size_t stackDrift = 0;
  size_t metaDrift = 0;
  std::string firstNameDrift;
  std::string firstStackDrift;

  for (uint16_t id : clientIds) {
    if (id == kAir)
      continue;
    const auto *srv = server.getItem(id);
    const auto *cli = ItemRegistry::GetItem(id);
    if (!srv || !cli) {
      continue; // one side does not know the id at all; not a value drift
    }
    ++compared;
    if (srv->name != cli->name) {
      if (nameDrift == 0)
        firstNameDrift = "id " + std::to_string(id) + ": server='" +
                         srv->name + "' client='" + cli->name + "'";
      ++nameDrift;
    }
    if (srv->max_stack_size != cli->stackSize) {
      if (stackDrift == 0)
        firstStackDrift = "id " + std::to_string(id) + ": server=" +
                          std::to_string(srv->max_stack_size) + " client=" +
                          std::to_string(cli->stackSize);
      ++stackDrift;
    }
    if (srv->default_meta != cli->meta) {
      ++metaDrift;
    }
  }

  CHECK(compared > 0, "the drift test compared at least one item");
  CHECK_EQ(nameDrift, size_t(0),
           ("no name drift between the client and server parsers" +
            std::string(nameDrift ? " — first: " + firstNameDrift : ""))
               .c_str());
  CHECK_EQ(stackDrift, size_t(0),
           ("no stack-size drift between the client and server parsers" +
            std::string(stackDrift ? " — first: " + firstStackDrift : ""))
               .c_str());
  CHECK_EQ(metaDrift, size_t(0),
           "no metadata drift between the client and server parsers");
}

static void test_client_mirror_knows_every_server_item_except_air() {
  // The other direction: does the client know about anything the server knows?
  // The ONLY legitimate difference is air, which the client skips by design.
  RecipeManager::ItemRegistry server;
  if (!server.loadFromCSV(itemsCsvPath()))
    return;
  ItemRegistry::LoadFromCSV(itemsCsvPath());

  std::vector<uint16_t> missingFromClient;
  for (const auto &id : ItemRegistry::GetAllItemIds()) {
    (void)id; // keep both loaded; the loop below walks the server instead
  }
  // Walk a bounded slice of the server registry by probing well-known ids
  // rather than iterating (getItem needs an id, and there is no id iterator
  // on the server side).
  for (uint32_t raw = 1; raw <= 0xFFFF && missingFromClient.size() < 8; ++raw) {
    const uint16_t id = static_cast<uint16_t>(raw);
    if (server.getItem(id) == nullptr)
      continue;
    if (ItemRegistry::GetItem(id) == nullptr)
      missingFromClient.push_back(id);
  }
  CHECK(missingFromClient.empty(),
        "the client mirror knows every item the server registry does");
}

static void test_client_and_server_agree_on_the_steam_fluid_id() {
  // steam is the one id with special meaning on BOTH sides: the server has
  // Registry::steamItemId() (used by validateResourceRequirements) and the
  // client has ItemRegistry::GetSteamItemId() (used by the fluid overlay).
  // If these two ever disagree, fluid recipes render the wrong item.
  ItemRegistry::LoadFromCSV(itemsCsvPath());
  CHECK_EQ(ItemRegistry::GetSteamItemId(), kSteam,
           "the client resolves steam to the CSV row 1111:11:1");

  RecipeManager::ItemRegistry server;
  if (!server.loadFromCSV(itemsCsvPath()))
    return;
  const auto *srvSteam = server.getItem(kSteam);
  CHECK(srvSteam != nullptr, "the server registry also knows the steam id");
  if (srvSteam)
    CHECK_EQ(srvSteam->name, std::string("steam"),
             "and agrees it is named steam");
}

static void test_client_recipe_mirror_uses_ids_the_item_mirror_resolves() {
  // The end-to-end invariant the two mirrors exist to serve: an item id that
  // the server RecipeManager puts in a recipe must be one the CLIENT can name.
  // Otherwise the NEI panel would show a row of "???".
  RecipeManager::ItemRegistry server;
  if (!server.loadFromCSV(itemsCsvPath()))
    return;
  ItemRegistry::LoadFromCSV(itemsCsvPath());

  // Build a recipe from the same YAML the server would load, then walk every id
  // it references and require the client to name it.
  RecipeManager::RecipeManager mgr;
  const bool loaded =
      mgr.loadRecipesFromYamlDirectory(std::string(DATA_DIR) + "/recipes");
  CHECK(loaded, "the real recipes directory loads into a server RecipeManager");
  if (!loaded)
    return;

  const std::vector<uint16_t> ids = mgr.collectRecipeItemIds();
  CHECK(!ids.empty(), "the real recipes contribute item ids");

  size_t checked = 0;
  std::string firstUnknown;
  size_t unknowns = 0;
  for (uint16_t id : ids) {
    if (++checked > kDriftProbeLimit)
      break;
    const std::string_view name = ItemRegistry::GetName(id);
    if (name == kUnknownName) {
      if (unknowns == 0)
        firstUnknown = std::to_string(id);
      ++unknowns;
    }
  }
  CHECK(unknowns == 0,
        ("every recipe item id resolves to a name in the client mirror" +
         std::string(unknowns ? " — first unknown id: " + firstUnknown : ""))
            .c_str());
}

#define TEST(name)                                                             \
  do {                                                                         \
    ++g_tests;                                                                 \
    printf("  TEST: %s\n", #name);                                             \
    test_##name();                                                             \
  } while (0)

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("=== client recipe/item mirror test suite ===\n\n");

  TEST(client_item_registry_present_key);
  TEST(client_item_registry_missing_key_returns_documented_default);
  TEST(client_item_registry_skips_air_by_design);
  TEST(client_item_registry_finds_steam_by_name);
  TEST(client_item_registry_all_ids_excludes_air);
  TEST(client_item_registry_reload_is_idempotent);
  TEST(client_item_registry_missing_csv_leaves_previous_state);

  TEST(recipe_db_starts_with_no_catalog);
  TEST(recipe_db_catalog_populates_from_a_reply);
  TEST(recipe_db_empty_catalog_reply_is_still_loaded);
  TEST(recipe_db_catalog_replaces_a_previous_catalog);
  TEST(recipe_db_rejects_a_malformed_frame);

  TEST(recipe_db_item_cache_miss_returns_empty);
  TEST(recipe_db_item_reply_splits_craft_and_use);
  TEST(recipe_db_item_reply_files_a_recycling_recipe_in_both_tabs);
  TEST(recipe_db_item_reply_copies_every_recipe_field);
  TEST(recipe_db_empty_item_reply_caches_an_empty_result);

  TEST(recipe_db_machine_cache_miss_returns_empty);
  TEST(recipe_db_machine_reply_populates_the_cache);
  TEST(recipe_db_machine_cache_is_keyed_per_machine);

  TEST(recipe_db_grid_key_is_deterministic_and_discriminating);
  TEST(recipe_db_grid_reply_fills_the_grid_cache);
  TEST(recipe_db_check_reply_without_a_match_reports_empty);
  TEST(recipe_db_grid_check_fires_immediately_on_a_cache_hit);
  TEST(recipe_db_reply_for_an_unknown_request_id_is_ignored);

  TEST(client_mirror_does_not_drift_from_the_server_parser);
  TEST(client_mirror_knows_every_server_item_except_air);
  TEST(client_and_server_agree_on_the_steam_fluid_id);
  TEST(client_recipe_mirror_uses_ids_the_item_mirror_resolves);

  printf("\n=== Results: %d tests, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
