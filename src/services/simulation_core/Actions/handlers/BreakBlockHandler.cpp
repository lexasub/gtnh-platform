#include "Actions/handlers/BreakBlockHandler.h"
#include "Actions/ActionContext.h"
#include "Actions/CasRunner.h"
#include "ECS/SimulationEngine.h"
#include "Network/IEventPublisher.h"
#include "Storage/IBlockRepository.h"
#include "Storage/PlayerInventoryStore.h"
#include "World/BlockDrops.h"
#include <spdlog/spdlog.h>
#include <array>

namespace simcore {

namespace {

// Dry-run: add every multiblock content stack into `inv` (a copy of the
// player's inventory). Mirrors PlayerInventoryStore::giveItem stacking
// (by item_id, max 64). Returns false and leaves `inv` in a partial state if
// anything does not fit — the caller must then NOT apply the change.
bool tryFitAll(std::array<PersistSlot, kInventorySlots>& inv,
               const std::vector<InventorySlot>& items) {
  constexpr uint8_t kMaxStack = 64;
  for (const auto& item : items) {
    if (item.item_id == 0) continue;
    int remaining = static_cast<int>(item.count);

    // Stack onto existing matching stacks first
    for (auto& s : inv) {
      if (remaining <= 0) break;
      if (s.item_id == item.item_id && s.count < kMaxStack) {
        uint8_t room = kMaxStack - s.count;
        uint8_t add = std::min(static_cast<uint8_t>(remaining), room);
        s.count = static_cast<uint8_t>(s.count + add);
        remaining -= add;
      }
    }
    // Then fill empty slots
    for (auto& s : inv) {
      if (remaining <= 0) break;
      if (s.item_id == 0) {
        uint8_t add = std::min(static_cast<uint8_t>(remaining), kMaxStack);
        s = {item.item_id, add, item.meta};
        remaining -= add;
      }
    }
    if (remaining > 0) return false;
  }
  return true;
}

} // namespace

bool BreakBlockHandler::canHandle(const ActionContext& ctx) const {
  return ctx.action_type == Protocol::PlayerActionType_LEFT_MOUSE_CLICK;
}

void BreakBlockHandler::handle(const ActionContext& ctx) const {
  // ── Multiblock block-break guard (task 2.1) ─────────────────────────────
  // Breaking ANY block of a multiblock returns ALL hatch+controller contents
  // to the breaking player. If they do not fit, refuse the break — the block
  // SHALL NOT break and no items are dropped.
  if (ctx.engine_ && ctx.inventoryStore_) {
    auto owner = ctx.engine_->findControllerAt(static_cast<uint32_t>(ctx.x),
                                               static_cast<uint32_t>(ctx.y),
                                               static_cast<uint32_t>(ctx.z));
    if (owner != ctx.engine_->getControllers().end()) {
      std::vector<simcore::InventorySlot> contents;
      ctx.engine_->collectControllerContents(owner->second, contents);
      auto inv = ctx.inventoryStore_->getSlots(ctx.player_id);
      if (!contents.empty() && !tryFitAll(inv, contents)) {
        spdlog::warn("Refusing to break multiblock at ({},{},{}): contents do not fit player {} inventory",
                     ctx.x, ctx.y, ctx.z, ctx.player_id);
        ctx.publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_REJECTED),
                                        ctx.x, ctx.y, ctx.z, ctx.expected_block_id, 0,
                                        "Multiblock contents do not fit in inventory",
                                        ctx.request_id,
                                        static_cast<uint8_t>(ctx.action_type));
        return;
      }
      if (!contents.empty()) {
        ctx.inventoryStore_->setSlots(ctx.player_id, inv);
        spdlog::info("Multiblock contents returned to player {} on break at ({},{},{})",
                     ctx.player_id, ctx.x, ctx.y, ctx.z);
      }
    }
  }

  uint16_t final_block_id = 0; // LEFT click breaks to air
  uint8_t final_meta = 0;

  auto onGiveItem = ctx.onGiveItem_;
  auto onDrillUse = ctx.onDrillUse_;
  auto publisher = ctx.publisher_;
  auto engine = ctx.engine_;
  uint64_t player_id = ctx.player_id;
  uint32_t request_id = ctx.request_id;
  uint16_t broken_block = ctx.eff_expected;

  runBlockCas(ctx, ctx.x, ctx.y, ctx.z, ctx.eff_expected, final_block_id,
              final_meta,
      [onGiveItem, onDrillUse, publisher, engine, player_id, request_id,
       eff_x = ctx.x, eff_y = ctx.y, eff_z = ctx.z, broken_block]() {
        if (broken_block != 0 && onGiveItem) {
          uint16_t drop_id = broken_block;
          uint8_t drop_count = 1;
          if (const auto* drops = BlockDrops::instance()) {
            if (const auto* d = drops->Get(broken_block)) {
              drop_id = d->result_id;
              drop_count = d->count;
            }
          }
          spdlog::info("Giving drop {} x{} to player {}", drop_id,
                       drop_count, player_id);
          onGiveItem(player_id, drop_id, drop_count, -1);
        }
        if (onDrillUse) {
          onDrillUse(player_id, eff_x, eff_y, eff_z, broken_block);
        }
        publisher->publishBlockChangedEvent(eff_x, eff_y, eff_z, 0, 0,
                                            request_id, player_id);
        if (engine) {
          engine->onBlockChanged(static_cast<uint32_t>(eff_x),
                                 static_cast<uint32_t>(eff_y),
                                 static_cast<uint32_t>(eff_z), 0, 0, 0);
        }
      });
}

} // namespace simcore
