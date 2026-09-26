#include <game/actions/handlers/PlaceBlockHandler.h>
#include <game/actions/ActionContext.h>
#include <game/actions/CasRunner.h>
#include <engine/sim/SimulationEngine.h>
#include <apps/simcore/Network/IEventPublisher.h>
#include <game/storage/IBlockRepository.h>
#include <game/storage/PlayerInventoryStore.h>
#include <game/world/BlockTransforms.h>
#include <data/registry/ToolIds.h>
#include <spdlog/spdlog.h>

namespace simcore {

namespace {

// The player's mode as THIS server has it, or the inventory store's own
// default for an unknown player when no store is attached. Read in exactly
// one place so canHandle() and RefusalReasonForMode() cannot disagree about
// what the mode was — a disagreement would let a frame be described as both
// placeable and refused.
uint8_t storedModeOf(const ActionContext& ctx) {
  return ctx.inventoryStore_ ? ctx.inventoryStore_->getGameMode(ctx.player_id)
                             : 0;  // store's own default for an unknown player
}

// The non-mode half of canHandle, factored out because RefusalReasonForMode
// has to reproduce it in order to decide whether the mode is even the reason
// this frame went unhandled. A frame that failed one of these is not a
// placement at all, and reporting a mode for it would be nonsense — e.g.
// "you may not place in SPECTATOR" for a frame that was a right-click on a
// furnace the machine handler already claimed.
bool isPlacementShape(const ActionContext& ctx) {
  return ctx.action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK &&
         ctx.held_item != 0 && !isMiningTool(ctx.held_item) &&
         ctx.held_item != ITEM_WRENCH;
}

} // namespace

const char* PlaceBlockHandler::RefusalReasonForMode(const ActionContext& ctx) {
  if (!isPlacementShape(ctx)) return nullptr;
  if (CanPlaceBlocksOnServer(storedModeOf(ctx))) return nullptr;
  return "game mode may not place blocks";
}

bool PlaceBlockHandler::canHandle(const ActionContext& ctx) const {
  // The shape checks first and the mode LAST, deliberately (gp-t71g).
  //
  // The shape checks are pure and cannot fail for a reason a log reader would
  // find interesting, while the mode is per-player server state. Running the
  // shape first means a frame that was never a placement is not rejected AS a
  // placement — the dispatcher must keep falling through to
  // MachineInteractHandler / ChestInteractHandler for a right-click on a
  // machine, and this gate must not shadow that.
  if (!isPlacementShape(ctx)) return false;
  return CanPlaceBlocksOnServer(storedModeOf(ctx));
}

void PlaceBlockHandler::handle(const ActionContext& ctx) const {
  uint16_t final_block_id = ctx.held_item;
  uint8_t final_meta = 0;
  if (const auto* transforms = BlockTransforms::instance()) {
    if (auto transform = transforms->Apply(ctx.eff_expected, final_block_id)) {
      final_block_id = transform->new_block_id;
      final_meta = transform->new_meta;
      spdlog::info("Block transformation applied: new_id={}", final_block_id);
    }
  }

  auto inventoryStore = ctx.inventoryStore_;
  auto publisher = ctx.publisher_;
  auto engine = ctx.engine_;
  auto onBlockPlaced = ctx.onBlockPlaced_;
  uint64_t player_id = ctx.player_id;
  uint32_t request_id = ctx.request_id;

  runBlockCas(ctx, ctx.eff_x, ctx.eff_y, ctx.eff_z, ctx.eff_expected,
              final_block_id, final_meta,
      [inventoryStore, publisher, engine, onBlockPlaced, player_id, request_id,
       eff_x = ctx.eff_x, eff_y = ctx.eff_y, eff_z = ctx.eff_z,
       placed_block = ctx.held_item, final_block_id, final_meta]() {
        if (placed_block != 0 && inventoryStore) {
          auto slots = inventoryStore->getSlots(player_id);
          for (auto& s : slots) {
            if (s.item_id == placed_block && s.count > 0) {
              s.count--;
              if (s.count == 0) s = {};
              spdlog::info("Placed block {} by player {} — consumed from inv",
                           placed_block, player_id);
              break;
            }
          }
          inventoryStore->setSlots(player_id, slots);
        }
        publisher->publishBlockChangedEvent(eff_x, eff_y, eff_z, final_block_id,
                                            final_meta, request_id, player_id);
        if (engine) {
          engine->onBlockChanged(static_cast<uint32_t>(eff_x),
                                 static_cast<uint32_t>(eff_y),
                                 static_cast<uint32_t>(eff_z), final_block_id,
                                 final_meta, 0);
        }
        if (onBlockPlaced) {
          onBlockPlaced(player_id, eff_x, eff_y, eff_z, final_block_id);
        }
      });
}

} // namespace simcore
