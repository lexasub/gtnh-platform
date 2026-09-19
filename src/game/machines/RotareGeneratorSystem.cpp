#include "RotareGeneratorSystem.h"
#include <spdlog/spdlog.h>

namespace simcore {

RotareGeneratorSystem::RotareGeneratorSystem(entt::registry& reg,
                                             std::shared_ptr<IEventPublisher> events,
                                             std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), events_(std::move(events)), pipeClient_(std::move(pipeClient))
{
}

void RotareGeneratorSystem::tick(float /*dt*/) {
    auto view = reg_.view<MachineComponent, EnergyStorage, RotareState>();

    for (auto ent : view) {
        auto& machine = view.get<MachineComponent>(ent);
        auto& energy  = view.get<EnergyStorage>(ent);
        auto& state   = view.get<RotareState>(ent);

        if (machine.machine_id != kRotareGeneratorBlockId) continue;
        if (!state.spinning) continue;
        if (state.remainingTicks <= 0) {
            state.spinning = false;
            continue;
        }

        int32_t space = energy.capacity - energy.current;
        int32_t toAdd = (state.energyPerTick < space) ? state.energyPerTick : space;
        if (toAdd <= 0) {
            state.spinning = false;
            continue;
        }

        energy.current += toAdd;
        state.remainingTicks--;

        if (pipeClient_) {
            pipeClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent),
                static_cast<int32_t>(machine.x),
                static_cast<int32_t>(machine.y),
                static_cast<int32_t>(machine.z),
                energy.current,
                energy.capacity,
                energy.maxInput,
                energy.maxOutput,
                energy.tier,
                static_cast<int32_t>(energy.type),
                true,
                false
            );
        }

        events_->publishBlockEntityUpdate(
            machine.x, machine.y, machine.z,
            machine.machine_id,
            {},
            static_cast<float>(state.remainingTicks) / kSpinDurationTicks,
            static_cast<uint32_t>(energy.current),
            energy.type);

        if (state.remainingTicks <= 0) {
            state.spinning = false;
            spdlog::info("Rotare generator stopped at ({},{},{})", machine.x, machine.y, machine.z);
        }
    }
}

void RotareGeneratorSystem::activate(entt::entity ent) {
    auto* state = reg_.try_get<RotareState>(ent);
    if (state && state->spinning) return;

    reg_.emplace_or_replace<RotareState>(ent, RotareState{true, kSpinDurationTicks, kEnergyPerTick});
}

} // namespace simcore
