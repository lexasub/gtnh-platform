#include "Actions/handlers/PlaceBlockHandler.h"
#include "Actions/ActionContext.h"
#include "Actions/CasRunner.h"
#include "ECS/SimulationEngine.h"
#include "Network/IEventPublisher.h"
#include "Storage/IBlockRepository.h"
#include "Storage/PlayerInventoryStore.h"
#include "World/BlockTransforms.h"
#include <data/registry/ToolIds.h>
#include <spdlog/spdlog.h>

namespace simcore {

bool PlaceBlockHandler::canHandle(const ActionContext& ctx) const {
  return ctx.action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK &&
         ctx.held_item != 0 && !isMiningTool(ctx.held_item) &&
         ctx.held_item != ITEM_WRENCH;
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
