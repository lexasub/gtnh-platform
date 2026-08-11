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

    int32_t x = flow->pos()->x();
    int32_t y = flow->pos()->y();
    int32_t z = flow->pos()->z();
    uint64_t from_node = flow->from_node_id();
    int32_t amount = flow->amount();
    uint32_t fluid_id = flow->fluid_id();
    if (from_node == 0 || amount <= 0) return;

    auto view = reg_.view<const Position>();
    for (auto entity : view) {
        auto& pos = view.get<const Position>(entity);
        if (static_cast<int32_t>(pos.x) != x ||
            static_cast<int32_t>(pos.y) != y ||
            static_cast<int32_t>(pos.z) != z)
            continue;

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
                spdlog::debug("FluidFlowHandler: machine at ({},{},{}) has no FLUID_IN face, fluid blocked", x, y, z);
                break;
            }

            int32_t accepted = fluid->addFluid(amount);
            if (fluidClient_) {
                fluidClient_->publishNodeUpdate(
                    from_node, mc ? mc->x : x, mc ? mc->y : y, mc ? mc->z : z,
                    fluid->fluid_id, fluid->amount, fluid->capacity,
                    0, 0, 0, false, true);
            }
            spdlog::trace("FluidFlowHandler: fluid {} x{} delivered to FluidStorage at ({},{},{}) accepted={}",
                          fluid_id, amount, x, y, z, accepted);
            break;
        }

        // Case 2: No FluidStorage, but has EnergyStorage with STEAM type — fluid-as-energy
        if (energy && energy->type == EnergyType::STEAM) {
            energy->current -= amount;
            if (energy->current < 0) energy->current = 0;
            if (mc && fluidClient_) {
                fluidClient_->publishNodeUpdate(
                    from_node, mc->x, mc->y, mc->z,
                    fluid_id, energy->current, energy->capacity,
                    0, 0, energy->tier, false, true);
            }
            spdlog::trace("FluidFlowHandler: fluid {} x{} consumed as steam energy at ({},{},{})",
                          fluid_id, amount, x, y, z);
            break;
        }

        // Case 2b: No STEAM EnergyStorage, but SteamOutputComponent — boiler steam pool
        if (auto* soc = reg_.try_get<SteamOutputComponent>(entity)) {
            soc->steam_stored -= amount;
            if (soc->steam_stored < 0) soc->steam_stored = 0;
            if (mc && fluidClient_) {
                fluidClient_->publishNodeUpdate(
                    from_node, mc->x, mc->y, mc->z,
                    fluid_id, static_cast<int32_t>(soc->steam_stored), static_cast<int32_t>(soc->steam_capacity),
                    0, 0, 0, true, false);              // is_source=true
            }
            spdlog::trace("FluidFlowHandler: fluid {} x{} drained from SteamOutputComponent at ({},{},{})",
                          fluid_id, amount, x, y, z);
            break;
        }

        // Case 3: No FluidStorage, no STEAM EnergyStorage — fluid blocked
        spdlog::debug("FluidFlowHandler: machine at ({},{},{}) has no FluidStorage or steam EnergyStorage, fluid blocked",
                      x, y, z);
        break;
    }
}

} // namespace simcore
