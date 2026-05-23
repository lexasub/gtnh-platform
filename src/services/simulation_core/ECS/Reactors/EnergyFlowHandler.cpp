#include "EnergyFlowHandler.h"
#include "ECS/components/EnergyStorage.h"
#include "ECS/components/MachineComponent.h"
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
    if (from_node == 0 || amount <= 0) return;

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
