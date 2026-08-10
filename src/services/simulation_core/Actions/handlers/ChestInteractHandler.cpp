#include "Actions/handlers/ChestInteractHandler.h"
#include "Actions/ActionContext.h"
#include "Network/IEventPublisher.h"
#include <spdlog/spdlog.h>

namespace simcore {

bool ChestInteractHandler::canHandle(const ActionContext& ctx) const {
  return ctx.is_chest &&
         ctx.action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK;
}

void ChestInteractHandler::handle(const ActionContext& ctx) const {
  spdlog::info("ChestInteractHandler: chest interact at ({},{},{})",
               ctx.x, ctx.y, ctx.z);
  // Publish the ack + OPEN_UI directive — this opens the client chest window.
  ctx.publisher_->publishBlockAck(
      static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
      ctx.x, ctx.y, ctx.z, ctx.expected_block_id, 0, nullptr, ctx.request_id,
      static_cast<uint8_t>(ctx.action_type));
  ctx.publisher_->publishBlockDirective(
      static_cast<uint8_t>(Protocol::BlockDirective_OPEN_UI),
      ctx.expected_block_id, ctx.x, ctx.y, ctx.z, ctx.request_id,
      static_cast<uint8_t>(ctx.action_type));
  // Chest contents are loaded by the chest.open round-trip (ChestOpenHandler
  // → container session → InventoryUpdate container_id=1), which also fills the
  // client window. The old LoadEntityState→BlockEntityUpdate path is removed
  // so there is exactly ONE chest read path.
}

} // namespace simcore
