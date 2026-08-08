#include "Actions/SetBlockCASHandler.h"
#include "Actions/ActionContext.h"
#include "Actions/handlers/BreakBlockHandler.h"
#include "Actions/handlers/ChestInteractHandler.h"
#include "Actions/handlers/MachineInteractHandler.h"
#include "Actions/handlers/PlaceBlockHandler.h"
#include "Storage/IBlockRepository.h"
#include "Network/IEventPublisher.h"
#include "Network/clients/EntityStateStoreClient.h"
#include "Storage/PlayerInventoryStore.h"
#include "ECS/SimulationEngine.h"
#include <spdlog/spdlog.h>

namespace simcore {

SetBlockCASHandler::SetBlockCASHandler(
    std::shared_ptr<IBlockRepository> repo,
    std::shared_ptr<IEventPublisher> publisher,
    std::shared_ptr<SimulationEngine> engine,
    std::shared_ptr<PlayerInventoryStore> inventoryStore,
    ItemGiveCallback onGiveItem, DrillUseCallback onDrillUse,
    BlockPlacedCallback onBlockPlaced, PostCallback postToMain)
    : repo_(std::move(repo)), publisher_(std::move(publisher)),
      engine_(std::move(engine)), inventoryStore_(std::move(inventoryStore)),
      onGiveItem_(std::move(onGiveItem)), onDrillUse_(std::move(onDrillUse)),
      onBlockPlaced_(std::move(onBlockPlaced)), postToMain_(std::move(postToMain))
{}

void SetBlockCASHandler::handle(const void* table) {
  handle(static_cast<const Protocol::SetBlockAction*>(table));
}

void SetBlockCASHandler::handle(const Protocol::SetBlockAction* action) {
  if (action == nullptr) {
    return;
  }

  ActionContext ctx(action, repo_, publisher_, engine_, inventoryStore_,
                    entityStateClient_, onGiveItem_, onDrillUse_,
                    onBlockPlaced_, postToMain_);

  if (dispatcher_.dispatch(ctx)) return;
  spdlog::info("Unhandled action: player={} type={} at ({},{},{})",
               ctx.player_id, static_cast<int>(ctx.action_type), ctx.x,
               ctx.y, ctx.z);
  if (ctx.publisher_) {
    ctx.publisher_->publishBlockAck(Protocol::BlockAckStatus_REJECTED, ctx.x,
                                    ctx.y, ctx.z, ctx.expected_block_id, 0,
                                    "nothing placeable in hand",
                                    ctx.request_id, ctx.action_type);
  }
}

} // namespace simcore
