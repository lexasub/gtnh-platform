#include "Actions/handlers/MachineInteractHandler.h"
#include "Actions/ActionContext.h"
#include "ECS/SimulationEngine.h"
#include "Network/IEventPublisher.h"
#include "Storage/IBlockRepository.h"
#include <data/registry/ToolIds.h>
#include <entt/entt.hpp>
#include <spdlog/spdlog.h>

namespace simcore {

namespace {

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
  handleMachineInteraction(ctx, ctx.x, ctx.y, ctx.z, ctx.expected_block_id,
                           ctx.player_id, ctx.request_id);
}

} // namespace simcore
