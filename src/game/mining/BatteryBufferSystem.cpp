#include "BatteryBufferSystem.h"
#include <engine/sim/components/Position.h>
#include <engine/sim/components/MachineComponent.h>
#include <algorithm>
#include <spdlog/spdlog.h>
#include <vector>

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

        if (events_) {
            if (auto* machine = m_registry.try_get<MachineComponent>(entity)) {
                std::vector<uint8_t> inventory_data;
                inventory_data.reserve(inv.slots.size() * 5);
                for (const auto& slot : inv.slots) {
                    inventory_data.push_back(static_cast<uint8_t>(slot.item_id));
                    inventory_data.push_back(static_cast<uint8_t>(slot.item_id >> 8));
                    inventory_data.push_back(slot.count);
                    inventory_data.push_back(static_cast<uint8_t>(slot.meta));
                    inventory_data.push_back(static_cast<uint8_t>(slot.meta >> 8));
                }
                events_->publishBlockEntityUpdate(
                    machine->x, machine->y, machine->z, machine->machine_id,
                    inventory_data, 1.0f, static_cast<uint32_t>(buffer.stored),
                    EnergyType::ELECTRICITY, buffer.capacity, buffer.numSlots);
            }
        }

        if (pipeClient_) {
            const auto* machine = m_registry.try_get<MachineComponent>(entity);
            if (machine) {
                pipeClient_->publishNodeUpdate(
                    static_cast<uint64_t>(entity), static_cast<int32_t>(pos.x),
                    static_cast<int32_t>(pos.y), static_cast<int32_t>(pos.z),
                    buffer.stored, static_cast<int32_t>(buffer.capacity),
                    buffer.maxInput, 0, buffer.tier,
                    static_cast<int32_t>(EnergyType::ELECTRICITY), false, true);
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
                        static_cast<int32_t>(EnergyType::ELECTRICITY),
                        needed
                    );
                    pendingRequests_[entity_id] = needed;
                    spdlog::trace("[BatteryBuffer] entity {} requested {} EU from PipeNetwork",
                                  entity_id, needed);
                }
            }
        }
    }
}

bool BatteryBufferSystem::onConsumeResponse(uint64_t node_id, int32_t consumed, int32_t) {
    if (consumed <= 0) return false;

    // Correlate strictly by node id (gp-u9ua).
    //
    // This used to read `if (node_id != 0) { ...direct... }` and, on a miss,
    // fall through to a FIFO fallback that credited pendingOrder_.front().
    // Both halves were wrong. entt hands id 0 to the first entity a registry
    // creates, and tick() sends that entity's own id as the request's node_id,
    // so node 0 is a real node on the wire — the guard could never take the
    // direct branch for it and every node-0 response was routed by guesswork:
    // with an empty queue the EU was dropped and the request never retired, and
    // with two buffers the FIFO front (the last-created entity, because the
    // view is walked in reverse) was credited instead of the addressed node.
    //
    // The FIFO fallback existed for a response that named no node, but no such
    // response is produced: PipeNetworkService::handleConsumeRequest echoes
    // req->node_id() into every reply (PipeNetworkService.cpp:959,1004), so a
    // missing id can only mean a request that was never sent. Credit nothing
    // and report the miss — that also lets SimCoreMessageHandler hand an
    // unclaimed response to the next system in the chain
    // (SimCoreMessageHandler.cpp:256-265) rather than having whichever system
    // is asked first guess, and it matches the sibling LCRSystem / EBFSystem
    // handlers, which already return false on an unknown id.
    auto it = pendingRequests_.find(node_id);
    if (it == pendingRequests_.end())
        return false;  // not our request (or already settled)

    if (auto* buffer = m_registry.try_get<BatteryBufferComponent>(
            static_cast<entt::entity>(node_id))) {
        buffer->stored = std::min(buffer->stored + consumed,
                                  static_cast<int32_t>(buffer->capacity));
    }
    // Retire the request even if the entity is gone or has lost its component:
    // the EU is spent, so leaving the entry behind would stall the buffer's
    // future requests forever.
    pendingRequests_.erase(it);
    return true;
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
