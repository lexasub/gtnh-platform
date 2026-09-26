#include "ExplosionSystem.h"
#include <game/machines/HeatSlowComponent.h>
#include <engine/sim/components/MachineComponent.h>
#include <game/machines/OverheatComponent.h>
#include <engine/sim/components/Position.h>
#include "HeatConstants.h"
#include <spdlog/spdlog.h>

namespace simcore {

void ExplosionSystem::tick(float /*dt*/) {
    // Candidate gate: MachineComponent + Position + OverheatComponent, and the
    // entity must be a live multiblock controller (MachineComponent::mb_id != 0).
    //
    // This used to be a 4-type view keyed on a `MultiblockController` ECS
    // component (gp-qgtc / gp-wjwr). That component is never emplaced anywhere
    // in production: SimulationEngine owns controllers in a plain
    // `std::unordered_map<uint64_t, MultiblockController> controllers_`
    // (SimulationEngine.h:108) which EBFSystem / LCRSystem / LargeBoilerSystem
    // mutate in place, so no ECS mirror could stay in sync with it. Keying off
    // `mb_id` instead uses the field the engine already maintains:
    //   * set to the controller id on formation  (SimulationEngine.cpp:309)
    //   * updated on every block-change echo     (SimulationEngine.cpp:370)
    //   * removed with the MachineComponent when the multiblock is destroyed
    //     (destroyController, SimulationEngine.cpp:82)
    // Only the anchor block is ever a machine (member blocks are casing/coil,
    // not machines), so `mb_id != 0` is exactly "is a controller anchor".
    auto view = reg_.view<MachineComponent, Position, OverheatComponent>();
    std::vector<entt::entity> toDestroy;

    for (auto ent : view) {
        auto& machine = view.get<MachineComponent>(ent);
        auto& overheat = view.get<OverheatComponent>(ent);
        auto& pos = view.get<Position>(ent);

        // Only multiblock controller anchors explode — single-block machines
        // are out of the view's original intent (see the note above).
        if (machine.mb_id == 0) continue;
        if (overheat.state != OverheatState::CRITICAL) continue;

        overheat.ticks_at_critical++;
        if (overheat.ticks_at_critical < HeatConstants::EXPLOSION_DELAY_TICKS) continue;

        spdlog::warn("[Explosion] Machine at ({},{},{}) exploded!", pos.x, pos.y, pos.z);

        events_->publishBlockChangedEvent(
            static_cast<int32_t>(pos.x),
            static_cast<int32_t>(pos.y),
            static_cast<int32_t>(pos.z),
            0, 0);

        toDestroy.push_back(ent);
    }

    for (auto ent : toDestroy) {
        reg_.destroy(ent);
    }
}

} // namespace simcore
