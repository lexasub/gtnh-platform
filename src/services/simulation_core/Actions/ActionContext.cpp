#include "Actions/ActionContext.h"
#include "ECS/SimulationEngine.h"
#include "Network/IEventPublisher.h"
#include "Storage/IBlockRepository.h"
#include "Storage/PlayerInventoryStore.h"
#include <data/registry/ToolIds.h>

namespace simcore {

ActionContext::ActionContext(const Protocol::SetBlockAction* action,
                             std::shared_ptr<IBlockRepository> repo,
                             std::shared_ptr<IEventPublisher> publisher,
                             std::shared_ptr<SimulationEngine> engine,
                             std::shared_ptr<PlayerInventoryStore> inventoryStore,
                             std::shared_ptr<EntityStateStoreClient> entityStateClient,
                             ItemGiveCallback onGiveItem, DrillUseCallback onDrillUse,
                             BlockPlacedCallback onBlockPlaced, PostCallback postToMain)
    : action(action)
    , repo_(std::move(repo))
    , publisher_(std::move(publisher))
    , engine_(std::move(engine))
    , inventoryStore_(std::move(inventoryStore))
    , entityStateClient_(std::move(entityStateClient))
    , onGiveItem_(std::move(onGiveItem))
    , onDrillUse_(std::move(onDrillUse))
    , onBlockPlaced_(std::move(onBlockPlaced))
    , postToMain_(std::move(postToMain)) {
  if (!action) return;
  action_type = action->action();
  if (auto* pos = action->pos()) {
    x = pos->x();
    y = pos->y();
    z = pos->z();
  }
  expected_block_id = action->expected_block_id();
  new_block_id = action->new_block_id();
  player_id = action->player_id();
  request_id = action->request_id();
  held_item = action->held_item();
  face = action->face();

  eff_x = x;
  eff_y = y;
  eff_z = z;
  eff_expected = expected_block_id;
  if (action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK) {
    faceAdjacentBlock(face, eff_x, eff_y, eff_z);
    eff_expected = 0; // place against air on the adjacent cell
  }

  is_chest = (expected_block_id == kChestBlockId);
  if (engine_) {
    if (auto* reg = engine_->getMachineRegistry()) {
      machine_info = reg->Get(expected_block_id);
    }
  }
}

} // namespace simcore
