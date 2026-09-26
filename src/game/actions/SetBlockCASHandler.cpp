#include <game/actions/SetBlockCASHandler.h>
#include <game/actions/ActionContext.h>
#include <game/actions/handlers/BreakBlockHandler.h>
#include <game/actions/handlers/ChestInteractHandler.h>
#include <game/actions/handlers/MachineInteractHandler.h>
#include <game/actions/handlers/PlaceBlockHandler.h>
#include <game/storage/IBlockRepository.h>
#include <apps/simcore/Network/IEventPublisher.h>
#include <apps/simcore/Network/clients/EntityStateStoreClient.h>
#include <game/storage/PlayerInventoryStore.h>
#include <engine/sim/SimulationEngine.h>
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

  spdlog::debug("SetBlockCAS: player={} type={} held=0x{:04X} expected=0x{:04X} new=0x{:04X} face={} req={} at ({},{},{}) eff ({},{},{}) effexp=0x{:04X}",
                ctx.player_id, static_cast<int>(ctx.action_type), ctx.held_item,
                ctx.expected_block_id, ctx.new_block_id, static_cast<int>(ctx.face),
                ctx.request_id, ctx.x, ctx.y, ctx.z,
                ctx.eff_x, ctx.eff_y, ctx.eff_z, ctx.eff_expected);
  if (dispatcher_.dispatch(ctx)) return;
  spdlog::info("Unhandled action: player={} type={} at ({},{},{})",
               ctx.player_id, static_cast<int>(ctx.action_type), ctx.x,
               ctx.y, ctx.z);
  if (ctx.publisher_) {
    // A frame can go unhandled for reasons that are not all the same, and the
    // client is owed the truth. PlaceBlockHandler declined this one because
    // the player's game mode may not place blocks (gp-t71g) — it does not claim
    // the frame precisely so that this ack still goes out, and reporting
    // "nothing placeable in hand" to a SPECTATOR who is holding a block would
    // be false and would point an operator at the client instead of the mode.
    // RefusalReasonForMode returns nullptr for every other cause, including a
    // frame an earlier handler already claimed, so the generic reason below is
    // still used for exactly the case it was written for.
    const char* reason = PlaceBlockHandler::RefusalReasonForMode(ctx);
    if (reason == nullptr) reason = "nothing placeable in hand";
    ctx.publisher_->publishBlockAck(Protocol::BlockAckStatus_REJECTED, ctx.x,
                                    ctx.y, ctx.z, ctx.expected_block_id, 0,
                                    reason, ctx.request_id, ctx.action_type);
  }
}

} // namespace simcore
