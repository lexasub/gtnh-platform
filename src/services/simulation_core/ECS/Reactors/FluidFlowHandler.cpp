#include "FluidFlowHandler.h"
#include "ECS/components/Position.h"
#include "ECS/components/Block.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/EnergyStorage.h"
#include "core_generated.h"
#include "pipe_network_generated.h"

namespace simcore {

FluidFlowHandler::FluidFlowHandler(entt::registry& reg,
                                   std::shared_ptr<FluidClient> fluidClient)
    : reg_(reg), fluidClient_(std::move(fluidClient))
{}

void FluidFlowHandler::handle(const std::vector<uint8_t>& data) {
    auto* flow = flatbuffers::GetRoot<Protocol::FluidFlowEvent>(data.data());
    if (!flow || !flow->pos()) return;

    int32_t x = flow->pos()->x();
    int32_t y = flow->pos()->y();
    int32_t z = flow->pos()->z();
    uint64_t from_node = flow->from_node_id();
    int32_t amount = flow->amount();
    if (from_node == 0 || amount <= 0) return;

    auto view = reg_.view<const Position>();
    for (auto entity : view) {
        auto& pos = view.get<const Position>(entity);
        if (static_cast<int32_t>(pos.x) == x &&
            static_cast<int32_t>(pos.y) == y &&
            static_cast<int32_t>(pos.z) == z) {

            auto* energy = reg_.try_get<EnergyStorage>(entity);
            auto* mc = reg_.try_get<MachineComponent>(entity);
            if (energy) {
                energy->current -= amount;
                if (energy->current < 0) energy->current = 0;
            }
            if (mc && energy && fluidClient_) {
                fluidClient_->publishNodeUpdate(
                    from_node, mc->x, mc->y, mc->z,
                    flow->fluid_id(), energy->current, energy->capacity,
                    0, 0, energy->tier, false, true);
            }
            break;
        }
    }
}

} // namespace simcore
