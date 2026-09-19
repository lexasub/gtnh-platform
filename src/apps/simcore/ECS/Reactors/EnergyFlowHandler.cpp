#include "EnergyFlowHandler.h"
#include <engine/sim/components/EnergyStorage.h>
#include <game/machines/BatteryBufferComponent.h>
#include <engine/sim/components/MachineComponent.h>
#include "core_generated.h"
#include "pipe_network_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

EnergyFlowHandler::EnergyFlowHandler(entt::registry& reg,
                                     std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), pipeClient_(std::move(pipeClient))
{}

void EnergyFlowHandler::handle(const std::vector<uint8_t>& data) {
    auto* flow = flatbuffers::GetRoot<Protocol::EnergyFlowEvent>(data.data());
    if (!flow || !flow->pos()) return;

    uint64_t from_node = flow->from_node_id();
    int32_t amount = flow->amount();
    if (amount <= 0) return;

    // from_node == 0 is generation sentinel for generic energy flows.
    // Battery buffers may legitimately use entity 0; handle that case explicitly
    // without treating the sentinel as an arbitrary entity.
    if (from_node == 0) {
        if (auto* buffer = reg_.try_get<simcore::BatteryBufferComponent>(entt::entity(0))) {
            buffer->stored -= amount;
            if (buffer->stored < 0) buffer->stored = 0;
            return;
        }
        // generation sentinel – no debit
        return;
    }

    // Battery buffers own separate stored-EU accounting; EnergyStorage does
    // not exist on them. Clamp is local underflow guard.
    if (auto* buffer = reg_.try_get<simcore::BatteryBufferComponent>(
            static_cast<entt::entity>(from_node))) {
        buffer->stored -= amount;
        if (buffer->stored < 0) buffer->stored = 0;
        return;
    }

    auto view = reg_.view<simcore::EnergyStorage>();
    for (auto entity : view) {
        if (static_cast<uint64_t>(entity) == from_node) {
            auto& es = view.get<simcore::EnergyStorage>(entity);
            es.current -= amount;
            if (es.current < 0) es.current = 0;

            auto* mc = reg_.try_get<simcore::MachineComponent>(entity);
            if (mc && pipeClient_) {
                pipeClient_->publishNodeUpdate(
                    from_node, mc->x, mc->y, mc->z,
                    es.current, es.capacity, es.maxInput, es.maxOutput,
                    es.tier, static_cast<int32_t>(es.type), true, false);
            }
            break;
        }
    }
}

} // namespace simcore
