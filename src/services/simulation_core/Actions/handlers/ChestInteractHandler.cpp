#include "Actions/handlers/ChestInteractHandler.h"
#include "Actions/ActionContext.h"
#include "Network/IEventPublisher.h"
#include "Network/clients/EntityStateStoreClient.h"
#include "machine_state_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <cstring>

namespace simcore {

bool ChestInteractHandler::canHandle(const ActionContext& ctx) const {
  return ctx.is_chest &&
         ctx.action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK;
}

void ChestInteractHandler::handle(const ActionContext& ctx) const {
  spdlog::info("ChestInteractHandler: chest interact at ({},{},{})",
               ctx.x, ctx.y, ctx.z);
  ctx.publisher_->publishBlockAck(
      static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
      ctx.x, ctx.y, ctx.z, ctx.expected_block_id, 0, nullptr, ctx.request_id,
      static_cast<uint8_t>(ctx.action_type));
  ctx.publisher_->publishBlockDirective(
      static_cast<uint8_t>(Protocol::BlockDirective_OPEN_UI),
      ctx.expected_block_id, ctx.x, ctx.y, ctx.z, ctx.request_id,
      static_cast<uint8_t>(ctx.action_type));

  // Load chest inventory from EntityStateStore and publish in BlockEntityUpdate
  auto publisher = ctx.publisher_;
  if (!ctx.entityStateClient_) {
      // No entity store — publish empty inventory
      std::vector<uint8_t> empty(27 * 5, 0);
      publisher->publishBlockEntityUpdate(ctx.x, ctx.y, ctx.z, ctx.expected_block_id,
                                          empty, 0.0f, 0);
      return;
  }
  ctx.entityStateClient_->LoadEntityState(0, ctx.x, ctx.y, ctx.z, kChestEntityType,
                                     [x = ctx.x, y = ctx.y, z = ctx.z, chest_id = ctx.expected_block_id,
                                         publisher](const EntityStateStoreClient::EntityStateData& state) {
         std::vector<uint8_t> inventory_data;
         if (!state.state.empty()) {
             auto verifier = flatbuffers::Verifier(state.state.data(), state.state.size());
             if (verifier.VerifyBuffer<Protocol::MachineState>(nullptr)) {
                 auto fbState = flatbuffers::GetRoot<Protocol::MachineState>(state.state.data());
                 auto* inv = fbState->inventory();
                 if (inv && inv->slots()) {
                     inventory_data.resize(inv->slots()->size() * 5);
                     uint8_t* ptr = inventory_data.data();
                     for (size_t i = 0; i < inv->slots()->size(); ++i) {
                         auto* s = inv->slots()->Get(i);
                         uint16_t id = s ? s->item_id() : 0;
                         uint8_t cnt = s ? static_cast<uint8_t>(s->count()) : 0;
                         uint16_t mt = s ? s->meta() : 0;
                         std::memcpy(ptr, &id, sizeof(uint16_t)); ptr += sizeof(uint16_t);
                         *ptr++ = cnt;
                         std::memcpy(ptr, &mt, sizeof(uint16_t)); ptr += sizeof(uint16_t);
                     }
                 }
             }
         }
         if (inventory_data.empty()) inventory_data.resize(27 * 5, 0);
         publisher->publishBlockEntityUpdate(x, y, z, chest_id, inventory_data, 0.0f, 0,
                                             EnergyType::ELECTRICITY, 0, 27);
     });
}

} // namespace simcore
