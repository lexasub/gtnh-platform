#include "BatteryBufferSystem.h"
#include "ECS/components/Position.h"
#include <spdlog/spdlog.h>

namespace simcore {

void BatteryBufferSystem::tick(float /*dt*/) {
    auto view = m_registry.view<BatteryBufferComponent, InventoryContainer, Position>();

    for (auto entity : view) {
        auto& buffer = view.get<BatteryBufferComponent>(entity);
        auto& inv = view.get<InventoryContainer>(entity);
        auto& pos = view.get<Position>(entity);

        for (uint8_t i = 0; i < buffer.numSlots && i < inv.slots.size(); i++) {
            if (inv.slots[i].item_id != 0) {
                chargeSlot(buffer, inv, i);
            }
        }

        if (pipeClient_ && buffer.stored < static_cast<int32_t>(buffer.capacity)) {
            uint64_t entity_id = static_cast<uint64_t>(entity);
            auto it = pendingRequests_.find(entity_id);
            if (it == pendingRequests_.end()) {
                int32_t space = static_cast<int32_t>(buffer.capacity) - buffer.stored;
                int32_t needed = std::min(space, buffer.maxInput);
                if (needed > 0) {
                    pipeClient_->sendConsumeRequest(
                        entity_id,
                        static_cast<int32_t>(pos.x),
                        static_cast<int32_t>(pos.y),
                        static_cast<int32_t>(pos.z),
                        0,
                        needed
                    );
                    pendingRequests_[entity_id] = needed;
                    pendingOrder_.push_back(entity_id);
                    spdlog::trace("[BatteryBuffer] entity {} requested {} EU from PipeNetwork",
                                  entity_id, needed);
                }
            }
        }
    }
}

bool BatteryBufferSystem::onConsumeResponse(uint64_t node_id, int32_t consumed, int32_t) {
    if (consumed <= 0) return false;
    if (node_id != 0) {
        auto it = pendingRequests_.find(node_id);
        if (it != pendingRequests_.end()) {
            auto* buffer = m_registry.try_get<BatteryBufferComponent>(static_cast<entt::entity>(node_id));
            if (buffer) {
                buffer->stored = std::min(buffer->stored + consumed, static_cast<int32_t>(buffer->capacity));
            }
            pendingRequests_.erase(it);
            return true;
        }
    }
    while (!pendingOrder_.empty()) {
        uint64_t oldest = pendingOrder_.front();
        pendingOrder_.pop_front();
        auto it = pendingRequests_.find(oldest);
        if (it != pendingRequests_.end()) {
            auto* buffer = m_registry.try_get<BatteryBufferComponent>(static_cast<entt::entity>(oldest));
            if (buffer) {
                buffer->stored = std::min(buffer->stored + consumed, static_cast<int32_t>(buffer->capacity));
            }
            pendingRequests_.erase(it);
            return true;
        }
    }
    return false;
}

void BatteryBufferSystem::chargeSlot(BatteryBufferComponent& buffer,
                                       InventoryContainer& inv, uint8_t slotIdx) {
    auto& slot = inv.slots[slotIdx];
    uint16_t itemId = slot.item_id;

    auto it = TOOL_ENERGY_DEFS.find(itemId);
    if (it == TOOL_ENERGY_DEFS.end()) return;

    const auto& def = it->second;
    simulation_core::ItemStack itemStack{slot.item_id, slot.count, slot.meta};
    int32_t currentEnergy = getToolEnergy(itemStack);

    if (currentEnergy >= def.capacity) return;

    int32_t energyToTransfer = std::min({
        buffer.chargeRate,
        buffer.stored,
        def.capacity - currentEnergy,
        def.maxInput
    });

    if (energyToTransfer <= 0) return;

    buffer.stored -= energyToTransfer;
    setToolEnergy(itemStack, currentEnergy + energyToTransfer);
    slot.meta = itemStack.meta;
}

} // namespace simcore
