#include "FluidFlowHandler.h"
#include "ECS/components/Position.h"
#include "ECS/components/Block.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/EnergyStorage.h"
#include "ECS/components/FluidStorage.h"
#include "ECS/components/SteamOutputComponent.h"
#include "core_generated.h"
#include "pipe_network_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

FluidFlowHandler::FluidFlowHandler(entt::registry& reg,
                                   std::shared_ptr<FluidClient> fluidClient)
    : reg_(reg), fluidClient_(std::move(fluidClient))
{}

void FluidFlowHandler::handle(const std::vector<uint8_t>& data) {
    auto* flow = flatbuffers::GetRoot<Protocol::FluidFlowEvent>(data.data());
    if (!flow || !flow->pos()) return;

    uint64_t from_node = flow->from_node_id();
    int32_t amount = flow->amount();
    uint32_t fluid_id = flow->fluid_id();
    if (from_node == 0 || amount <= 0) return;

    // Match entity by from_node_id (= ECS entity id), like EnergyFlowHandler does.
    // This is reliable regardless of which position the flow event carries.
    entt::entity entity = static_cast<entt::entity>(from_node);
    if (!reg_.valid(entity)) {
        spdlog::trace("FluidFlowHandler: entity {} from from_node_id not valid", from_node);
        return;
    }

    auto* mc = reg_.try_get<MachineComponent>(entity);
    auto* energy = reg_.try_get<EnergyStorage>(entity);
    auto* fluid = reg_.try_get<FluidStorage>(entity);

    // Case 1: Machine has FluidStorage component — fluid-as-fluid delivery
    if (fluid) {
        bool faceAllowsFluid = true;
        if (mc) {
            faceAllowsFluid = false;
            for (int f = 0; f < 6; ++f) {
                uint8_t role = mc->getFaceRole(f);
                if (role == static_cast<uint8_t>(MachineFaceRole::FLUID_IN) ||
                    role == static_cast<uint8_t>(MachineFaceRole::NONE)) {
                    faceAllowsFluid = true;
                    break;
                }
            }
        }
        if (!faceAllowsFluid) {
            spdlog::debug("FluidFlowHandler: machine at from_node {} has no FLUID_IN face, fluid blocked",
                          from_node);
            return;
        }

        int32_t accepted = fluid->addFluid(amount);
        if (fluidClient_) {
            fluidClient_->publishNodeUpdate(
                from_node, mc ? mc->x : 0, mc ? mc->y : 0, mc ? mc->z : 0,
                fluid->fluid_id, fluid->amount, fluid->capacity,
                0, 0, 0, false, true);
        }
        spdlog::trace("FluidFlowHandler: fluid {} x{} delivered to FluidStorage at from_node {} accepted={}",
                      fluid_id, amount, from_node, accepted);
        return;
    }

    // Case 2: STEAM EnergyStorage. A flow event with no destination is emitted
    // for a source that PipeNetwork already debited, so mirror that debit in the
    // owning solid boiler's ECS state. Consumer credit is performed exclusively
    // by MachineSystem::onFluidConsumeResponse; crediting it here as well would
    // mint steam whenever a source event is replayed.
    if (energy && energy->type == EnergyType::STEAM) {
        if (flow->to_node_id() == 0) {
            energy->current -= amount;
            if (energy->current < 0) energy->current = 0;
            if (mc && fluidClient_) {
                fluidClient_->publishNodeUpdate(
                    from_node, mc->x, mc->y, mc->z,
                    fluid_id, energy->current, energy->capacity,
                    0, 0, energy->tier, true, false);
            }
            spdlog::trace("FluidFlowHandler: fluid {} x{} drained from STEAM EnergyStorage at node {} (remaining: {})",
                          fluid_id, amount, from_node, energy->current);
        }
        return;
    }

    // Case 3: SteamOutputComponent — boiler steam pool (source was drained)
    if (auto* soc = reg_.try_get<SteamOutputComponent>(entity)) {
        soc->steam_stored -= amount;
        if (soc->steam_stored < 0) soc->steam_stored = 0;
        if (mc && fluidClient_) {
            fluidClient_->publishNodeUpdate(
                from_node, mc->x, mc->y, mc->z,
                fluid_id, static_cast<int32_t>(soc->steam_stored),
                static_cast<int32_t>(soc->steam_capacity),
                0, 0, 0, true, false);
        }
        spdlog::trace("FluidFlowHandler: fluid {} x{} drained from SteamOutputComponent at node {} (remaining: {})",
                      fluid_id, amount, from_node, static_cast<int32_t>(soc->steam_stored));
        return;
    }

    // Case 4: No FluidStorage, no STEAM EnergyStorage, no SteamOutputComponent
    spdlog::debug("FluidFlowHandler: entity {} has no relevant fluid/steam component, fluid blocked",
                  from_node);
}

} // namespace simcore
