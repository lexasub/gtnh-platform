#include <game/actions/handlers/MachineInteractHandler.h>
#include <game/actions/ActionContext.h>
#include <engine/sim/SimulationEngine.h>
#include <apps/simcore/Network/IEventPublisher.h>
#include <game/storage/IBlockRepository.h>
#include <game/storage/PlayerInventoryStore.h>
#include <game/recipes/ItemRegistry.h>
#include <engine/sim/components/FluidStorage.h>
#include <data/registry/ToolIds.h>
#include <entt/entt.hpp>
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace simcore {

namespace {

// The empty bucket that is consumed and replaced, and the volume one fill
// takes out of the machine. Both from the table's own comment header: the
// consumed bucket is 0:11111:3 (empty_bucket, items.csv:237) and a bucket is
// 1000 mB.
constexpr uint16_t kEmptyBucket = ItemId::pack("0:11111:3");
constexpr int32_t kBucketVolume = 1000;

entt::entity findEntityAt(const std::shared_ptr<SimulationEngine>& engine,
                          int32_t x, int32_t y, int32_t z) {
  if (!engine) return entt::null;
  auto& reg = engine->reg();
  auto vw = reg.view<const simcore::Position>();
  for (auto e : vw) {
    auto& pp = vw.get<const simcore::Position>(e);
    if (static_cast<int32_t>(pp.x) == x &&
        static_cast<int32_t>(pp.y) == y &&
        static_cast<int32_t>(pp.z) == z) return e;
  }
  return entt::null;
}

void publishMachineState(const std::shared_ptr<SimulationEngine>& engine,
                         const std::shared_ptr<IEventPublisher>& publisher,
                         int32_t x, int32_t y, int32_t z,
                         uint16_t machine_id, uint64_t player_id,
                         uint32_t request_id) {
  engine->onMachineInteracted(x, y, z, machine_id, player_id);

  // Report the entity's real state so the client's MachineWindow opens with
  // the correct energy type/level (not a hardcoded 0/EU).
  auto* machineReg = engine->getMachineRegistry();
  EnergyType etype = EnergyType::ELECTRICITY;
  uint32_t energy = 0;
  uint32_t capacity = 0;
  int slotsIn = -1;

  if (auto ent = findEntityAt(engine, x, y, z); ent != entt::null) {
    auto& r = engine->reg();
    if (auto* es = r.try_get<simcore::EnergyStorage>(ent)) {
      etype = es->type;
      energy = static_cast<uint32_t>(es->current);
      capacity = static_cast<uint32_t>(es->capacity);
    }
    if (auto* mc = r.try_get<simcore::MachineComponent>(ent)) {
      if (machineReg) {
        if (auto* info = machineReg->Get(mc->machine_id)) {
          slotsIn = info->slots_in;
        }
      }
    }
  } else if (machineReg) {
    if (auto* info = machineReg->Get(machine_id)) {
      if (info->energy_in.has_value()) etype = info->energy_in.value();
      else if (info->energy_out.has_value()) etype = info->energy_out.value();
    }
  }

  publisher->publishBlockEntityUpdate(x, y, z, machine_id, {}, 0.0f, energy, etype, capacity, slotsIn);
  publisher->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
                             x, y, z, machine_id, 0, "Machine interacted", request_id);
  publisher->publishBlockDirective(static_cast<uint8_t>(Protocol::BlockDirective_OPEN_UI),
                                   machine_id, x, y, z, request_id);
}

// ── Fluid -> bucket (gp-jmlq, design (b)) ─────────────────────────────────

// The filled-bucket item NAME for the fluid sitting in `fluid`, or nullptr
// when the fluid has no row in the table. A fluid with no bucket row means the
// click does nothing and the machine is NOT drained — which is what should
// happen rather than conjuring a bucket that does not exist.
//
// Returns false (with the machine untouched) when the machine cannot supply a
// full bucket: less fluid than one bucket holds, or a max_output that would
// clamp the removal into a partial drain. Refusing a partial drain is the point
// of the second test — removeFluid() clamps, so asking it for 1000 mB out of a
// machine rated for 500 would silently take 500 and hand over a whole bucket.
bool planBucketFill(const std::shared_ptr<SimulationEngine>& engine,
                    entt::entity entity, uint16_t& filled_bucket_id) {
  if (!engine || entity == entt::null) return false;

  auto& reg = engine->reg();
  const auto* fluid = reg.try_get<simcore::FluidStorage>(entity);
  if (fluid == nullptr) return false;
  if (fluid->fluid_id == 0) return false;  // an empty buffer is not a fluid
  if (fluid->amount < kBucketVolume) return false;
  if (fluid->maxOutput < kBucketVolume) return false;

  auto* table = FluidBuckets::instance();
  if (table == nullptr) return false;
  const std::string* bucket_name = table->Get(fluid->fluid_id);
  if (bucket_name == nullptr) return false;

  // hasName(), NOT nameToId(...) == 0: id 0 is a VALID id (air, 0:0:0 in
  // items.csv), so a failed lookup is indistinguishable from a real id 0 and
  // would hand the player something nobody asked for (the gp-hmb0 lesson).
  const auto& items = RecipeManager::ItemRegistry::instance();
  if (!items.hasName(*bucket_name)) {
    spdlog::warn(
        "MachineInteractHandler: fluid {} maps to bucket \"{}\", which no item "
        "declares — the click is inert and the machine keeps its fluid",
        fluid->fluid_id, *bucket_name);
    return false;
  }
  filled_bucket_id = items.nameToId(*bucket_name);
  if (filled_bucket_id == 0) {
    // hasName passed, so this really is the item whose id is 0. There is no
    // bucket with id 0, so refuse rather than hand out air and drain 1000 mB.
    spdlog::warn(
        "MachineInteractHandler: bucket \"{}\" for fluid {} resolves to id 0 "
        "(air) — refusing to drain the machine for it",
        *bucket_name, fluid->fluid_id);
    return false;
  }
  return true;
}

// Take one empty bucket out of the player and report which slot it came from.
// Returns -1 when the player is not actually holding one.
//
// SetBlockAction carries `held_item` but NOT the slot it came from (core.fbs:
//142-153), so the server cannot address the hand directly. Scanning for the
// first empty-bucket stack is the same approach PlaceBlockHandler takes to
// charge for a placed block (PlaceBlockHandler.cpp:92-103).
int32_t consumeEmptyBucket(const std::shared_ptr<PlayerInventoryStore>& inv,
                           uint64_t player_id) {
  if (!inv) return -1;
  auto slots = inv->getSlots(player_id);
  for (size_t i = 0; i < slots.size(); ++i) {
    if (slots[i].item_id != kEmptyBucket || slots[i].count == 0) continue;
    if (slots[i].count > 1) {
      slots[i].count--;
    } else {
      slots[i] = {};
    }
    inv->setSlots(player_id, slots);
    return static_cast<int32_t>(i);
  }
  return -1;
}

// Right-click a machine with an empty bucket in hand: take 1000 mB of the
// fluid out of the machine and put the matching filled bucket where the empty
// one was.
//
// Returns true only when the fill actually happened; the caller falls through
// to the normal machine interaction on false, so every way this can go wrong
// is a plain no-op on an otherwise ordinary machine click.
//
// ORDER OF OPERATIONS — the safety-critical part. The machine loses fluid ONLY
// when the player really ends up holding a filled bucket:
//   1. every precondition is checked first, all of them read-only;
//   2. removeFluid(), and the result is VERIFIED to be a whole bucket;
//   3. the empty bucket is consumed out of the slot it was in;
//   4. the filled bucket is granted INTO THAT SAME SLOT.
//
// Steps 3-4 cannot fail once step 2 has returned 1000: the slot was located in
// step 1, and freeing it is what makes room, so giveItem's targeted-slot branch
// (PlayerInventoryStore.cpp:99-118) always has somewhere to write. Draining
// first and granting to an arbitrary free slot — the obvious implementation —
// would destroy the fluid whenever the inventory is full and the grant is
// dropped, because the ItemGiveCallback is void: the production lambda in
// SimCoreMessageHandler.cpp:160-162 discards giveItem()'s bool, so a failed
// grant is invisible to a caller that has already drained.
bool tryFillBucket(const ActionContext& ctx) {
  if (ctx.action_type != Protocol::PlayerActionType_RIGHT_MOUSE_CLICK) {
    return false;
  }
  if (ctx.held_item != kEmptyBucket) return false;
  if (!ctx.engine_) return false;
  // Both halves of the grant path must exist before anything is drained.
  if (!ctx.inventoryStore_ || !ctx.onGiveItem_) return false;

  const entt::entity entity =
      findEntityAt(ctx.engine_, ctx.x, ctx.y, ctx.z);
  if (entity == entt::null) return false;

  int32_t slot_index = -1;
  uint16_t filled_bucket_id = 0;
  if (!planBucketFill(ctx.engine_, entity, filled_bucket_id)) {
    return false;
  }
  // The hand must really hold a bucket: ctx.held_item is client-supplied and
  // the authoritative slots are the server's own.
  const int32_t held_slot =
      consumeEmptyBucket(ctx.inventoryStore_, ctx.player_id);
  if (held_slot < 0) return false;
  slot_index = held_slot;

  auto* fluid = ctx.engine_->reg().try_get<simcore::FluidStorage>(entity);
  if (fluid == nullptr) return false;
  const int32_t removed = fluid->removeFluid(kBucketVolume);
  if (removed < 0) {
    // Unreachable once planBucketFill has accepted this buffer (single-threaded
    // on the sim main thread, amount and maxOutput both already checked), but
    // a partial drain with no bucket to show for it is the one outcome this
    // whole ordering exists to prevent, so it is checked rather than assumed.
    spdlog::error(
        "MachineInteractHandler: asked for {} mB, the machine gave {} — the "
        "bucket is not granted",
        kBucketVolume, removed);
    fluid->addFluid(removed);  // put back what was taken
    return false;
  }

  ctx.onGiveItem_(ctx.player_id, filled_bucket_id, 1, slot_index);
  spdlog::info(
      "MachineInteractHandler: filled bucket {} from fluid {} at ({},{},{}), "
      "drained {} mB into slot {}",
      filled_bucket_id, fluid->fluid_id, ctx.x, ctx.y, ctx.z, removed,
      slot_index);
  return true;
}

void handleMachineInteraction(const ActionContext& ctx, int32_t x, int32_t y,
                              int32_t z, uint16_t machine_id,
                              uint64_t player_id, uint32_t request_id) {
  // A machine the player right-clicks may predate this simcore instance
  // (persisted in ChunkStore before a restart). ECS machine entities are
  // created ONLY on block-change events (onBlockChanged), so such machines
  // have no entity — and an entity-less machine is invisible to
  // GeneratorSystem, MachineSystem, and AdjacencyTransferSystem (heat can never
  // reach a furnace that has no entity). Lazily create it from ChunkStore,
  // mirroring MachineSlotHandler.
  if (findEntityAt(ctx.engine_, x, y, z) != entt::null) {
    publishMachineState(ctx.engine_, ctx.publisher_, x, y, z, machine_id,
                        player_id, request_id);
    return;
  }

  spdlog::warn("MachineInteractHandler: no ECS entity for machine {} at ({},{},{}) — lazy-init from ChunkStore",
               machine_id, x, y, z);
  auto repo = ctx.repo_;
  auto engine = ctx.engine_;
  auto publisher = ctx.publisher_;
  repo->getBlock(x, y, z,
      [x, y, z, machine_id, player_id, request_id, repo, engine, publisher](const BlockData& bd) {
        uint16_t finalId = machine_id;
        if (bd.block_id != 0) {
          finalId = bd.block_id;
          engine->onBlockChanged(static_cast<uint32_t>(x),
                                 static_cast<uint32_t>(y),
                                 static_cast<uint32_t>(z),
                                 bd.block_id, bd.meta, bd.mb_id);
          spdlog::info("[SimCore] Lazy-created ECS entity at ({},{},{}) block_id={}",
                       x, y, z, bd.block_id);
        }
        // The actual block may no longer be what the client expected —
        // only treat this as a machine interaction if it really is one.
        auto* machineReg = engine->getMachineRegistry();
        if (!machineReg || !machineReg->IsMachine(finalId)) {
          spdlog::warn("MachineInteractHandler: block {} at ({},{},{}) is not a machine — reject",
                       finalId, x, y, z);
          publisher->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_REJECTED),
                                     x, y, z, finalId, 0,
                                     "Block is not a machine", request_id);
          return;
        }
        publishMachineState(engine, publisher, x, y, z, finalId, player_id,
                            request_id);
      });
}

} // namespace

// ===========================================================================
// FluidBuckets — the fluid -> filled-bucket table (gp-jmlq)
// ===========================================================================

FluidBuckets* FluidBuckets::instance_ = nullptr;

namespace {

// ItemId::pack() scans for digits and yields 0 when it finds none
// (ItemId.h:85-131), so a typo'd fluid id is indistinguishable from a
// deliberate one by value alone — and 0 is a real fluid-less buffer, not a
// fluid. The two literal spellings of air are therefore the only zero a row
// may carry, the same rule BlockDrops.cpp:18-20 applies to its id columns.
bool isAirIdLiteral(const std::string& s) {
  return s == "0" || s == "0:0:0";
}

} // namespace

FluidBuckets* FluidBuckets::Load(const char* csv_path) {
  auto table = new FluidBuckets{};
  std::ifstream file(csv_path);
  if (!file.is_open()) {
    // An unreadable table is an EMPTY table, not a null one: every bucket
    // click then falls through as inert, which is the documented behavior for
    // a fluid with no row. A null here would mean a null-deref instead.
    spdlog::error("FluidBuckets: cannot open {}", csv_path);
    return table;
  }

  std::string line;
  size_t lineNum = 0;
  while (std::getline(file, line)) {
    ++lineNum;
    if (line.empty() || line[0] == '#') continue;

    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) cols.push_back(cell);
    if (cols.size() < 2) continue;

    // The header row (`fluid_id,bucket_id`) is a real line and must be
    // skipped by name, exactly as ItemRegistry.cpp:44-47 skips items.csv's.
    // Without this, pack("fluid_id") finds no digits, yields 0, and every
    // single load logs a bogus "invalid fluid id" warning about a file that
    // is perfectly fine — a false alarm that trains operators to ignore this
    // logger.
    if (cols[0] == "fluid_id") continue;

    const uint32_t fluid_id = ItemId::pack(cols[0]);
    if (fluid_id == 0 && !isAirIdLiteral(cols[0])) {
      spdlog::warn("FluidBuckets: invalid fluid id \"{}\" — skipping line {}",
                   cols[0], lineNum);
      continue;
    }
    if (cols[1].empty()) {
      spdlog::warn(
          "FluidBuckets: empty bucket name for fluid {} at line {} — skipping",
          cols[0], lineNum);
      continue;
    }

    // The bucket column is an item NAME, resolved through ItemRegistry at the
    // moment of the click. It is deliberately NOT packed here: an unknown name
    // must stay unknown until a click asks about it, and packing it now would
    // collapse "no such item" into id 0 — air — and make the fill free.
    // emplace-first-wins, so a duplicate fluid row cannot shadow the first.
    table->buckets_.emplace(fluid_id, cols[1]);
  }
  spdlog::info("FluidBuckets: loaded {} fluid->bucket rules from {}", table->size(),
               csv_path);
  return table;
}

FluidBuckets* FluidBuckets::instance() {
  if (instance_ == nullptr) {
    instance_ = Load(kDefaultCsvPath);
  }
  return instance_;
}

const std::string* FluidBuckets::Get(uint32_t fluid_id) const {
  auto it = buckets_.find(fluid_id);
  return it == buckets_.end() ? nullptr : &it->second;
}

bool MachineInteractHandler::canHandle(const ActionContext& ctx) const {
  if (!ctx.engine_ || !ctx.machine_info) return false;
  // Machine takes priority over placement (tuple order). held_item is
  // intentionally NOT filtered for RIGHT click: a machine's GUI opens
  // regardless of the equipped tool. LEFT click only interacts when the
  // machine opts in (interact_on_left) and the hand holds no mining tool.
  if (ctx.action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK) {
    return true;
  }
  if (ctx.action_type == Protocol::PlayerActionType_LEFT_MOUSE_CLICK) {
    return ctx.machine_info->interact_on_left && !isMiningTool(ctx.held_item);
  }
  return false;
}

void MachineInteractHandler::handle(const ActionContext& ctx) const {
  if (ctx.action_type == Protocol::PlayerActionType_LEFT_MOUSE_CLICK) {
    spdlog::info("MachineInteractHandler: left-click spin machine {} at ({},{},{})",
                 ctx.expected_block_id, ctx.x, ctx.y, ctx.z);
    ctx.engine_->onMachineInteracted(ctx.x, ctx.y, ctx.z, ctx.expected_block_id,
                                     ctx.player_id);
    ctx.publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
                                    ctx.x, ctx.y, ctx.z, ctx.expected_block_id, 0,
                                    "Machine spun", ctx.request_id,
                                    static_cast<uint8_t>(ctx.action_type));
    ctx.publisher_->publishBlockDirective(
        static_cast<uint8_t>(Protocol::BlockDirective_PLAY_ANIMATION),
        1 /* spin effect */, ctx.x, ctx.y, ctx.z, ctx.request_id,
        static_cast<uint8_t>(ctx.action_type));
    return;
  }
  // Bucket fill first, for RIGHT clicks only (gp-jmlq). The left-click branch
  // above is the spin, and a bucket is not a mining tool, so the two can never
  // both want the frame.
  //
  // Placed BEFORE the generic path so a machine that CAN hand over a filled
  // bucket does not also open its window — the player asked to fill a bucket,
  // not to look inside. tryFillBucket() is a no-op returning false for every
  // other case, so an ordinary right-click falls through untouched to the
  // normal machine interaction below.
  if (tryFillBucket(ctx)) return;

  handleMachineInteraction(ctx, ctx.x, ctx.y, ctx.z, ctx.expected_block_id,
                           ctx.player_id, ctx.request_id);
}

} // namespace simcore
