#include "CreativeGeneratorSystem.h"
#include <spdlog/spdlog.h>

namespace simcore {

CreativeGeneratorSystem::CreativeGeneratorSystem(entt::registry& reg,
                                                 std::shared_ptr<IEventPublisher> events,
                                                 std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), events_(events), pipeClient_(pipeClient)
{
}

void CreativeGeneratorSystem::tick(float /*dt*/) {
    auto view = reg_.view<MachineComponent, EnergyStorage>();

    for (auto ent : view) {
        auto& machine = view.get<MachineComponent>(ent);
        auto& energy  = view.get<EnergyStorage>(ent);

        if (machine.machine_id != kCreativeGeneratorBlockId) continue;
        if (energy.isFull()) continue;

        int32_t space = energy.capacity - energy.current;
        int32_t toAdd = (energyPerTick_ < space) ? energyPerTick_ : space;
        if (toAdd <= 0) continue;

        energy.current += toAdd;

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
                true,   // is_source
                false   // is_sink
            );
        }

        events_->publishBlockEntityUpdate(
            machine.x, machine.y, machine.z,
            machine.machine_id,
            {},
            1.0f,
            static_cast<uint32_t>(energy.current),
            energy.type);
    }
}

} // namespace simcore
