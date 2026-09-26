#include "RotareGeneratorSystem.h"
#include <engine/sim/components/Position.h>
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
            energy.type,
            // The ROTATION buffer capacity, like every sibling machine system
            // (GeneratorSystem.cpp:206, EBFSystem.cpp:331, LCRSystem.cpp:252,
            // MachineSystem.cpp:109,555). Omitting it falls through to the
            // IEventPublisher default of 0, and MachineWindow.cpp:426-428 only
            // trusts a wire capacity when it is > 0 — a tier-0 rotare_generator
            // would then be drawn against a tier-derived 10000 EU bar instead of
            // its declared 5000.
            static_cast<uint32_t>(energy.capacity));

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

void registerRotareInteraction(SimulationEngine& engine, RotareGeneratorSystem& system) {
    engine.registerMachineInteractionHandler(
        RotareGeneratorSystem::kRotareGeneratorBlockId,
        [&system](int32_t x, int32_t y, int32_t z, uint64_t /*player_id*/) {
            // Resolve the clicked block on the SYSTEM's registry, not the
            // engine's: the system ticks that registry, so it is the one that
            // decides which RotareState it can see. They are the same registry
            // in production (main.cpp hands both simulationEngine->reg()).
            auto& reg = system.registry();
            entt::entity target = entt::null;
            for (auto entity : reg.view<const simcore::Position>()) {
                const auto& pos = reg.get<const simcore::Position>(entity);
                if (static_cast<int32_t>(pos.x) == x &&
                    static_cast<int32_t>(pos.y) == y &&
                    static_cast<int32_t>(pos.z) == z) {
                    target = entity;
                    break;
                }
            }
            // No entity, or one the tick loop cannot see: the click is still
            // acked and animated by MachineInteractHandler, but there is
            // nothing here to spin until the block-change event creates it.
            if (target == entt::null) return false;
            if (!reg.all_of<MachineComponent, EnergyStorage>(target)) return false;

            const auto& machine = reg.get<MachineComponent>(target);
            if (machine.machine_id != RotareGeneratorSystem::kRotareGeneratorBlockId) {
                return false;
            }

            system.activate(target);
            return true;
        });
}

} // namespace simcore
