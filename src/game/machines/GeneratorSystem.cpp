#include "GeneratorSystem.h"
#include "Network/FluidClient.h"
#include "content/content.h"
#include "MachineRegistry.h"
#include <engine/sim/components/HeatIntakeComponent.h>
#include <engine/sim/components/SteamOutputComponent.h>
#include <cstring>
#include <spdlog/spdlog.h>

namespace simcore {

namespace {
    inline bool isGenerator(uint16_t block_id) {
        return block_id == content::kGeneratorMachineId_Coal || block_id == content::kGeneratorMachineId_Steam;
    }
}

const std::unordered_map<uint16_t, int32_t>& GeneratorSystem::FuelValues() {
    return content::generatorFuelValues();
}

GeneratorSystem::GeneratorSystem(entt::registry& reg,
                                 std::shared_ptr<IEventPublisher> events,
                                 std::shared_ptr<PipeEnergyClient> pipeClient,
                                 std::shared_ptr<FluidClient> fluidClient,
                                 std::uint16_t steam_item_id)
    : reg_(reg), events_(events), pipeClient_(pipeClient), fluidClient_(fluidClient),
      steam_id_(steam_item_id)
{
}

void GeneratorSystem::tick(float /*dt*/) {
    auto view = reg_.view<MachineComponent, InventoryContainer, EnergyStorage>();

    for (auto ent : view) {
        auto& machine = view.get<MachineComponent>(ent);
        auto& container = view.get<InventoryContainer>(ent);
        auto& energy = view.get<EnergyStorage>(ent);

        if (!isGenerator(machine.machine_id)) continue;

        // Fail-closed: without a resolved Steam id the produced steam could
        // never be identified or drained — do not burn fuel into a stranded
        // buffer and do not advertise a steam source.
        if (energy.type == EnergyType::STEAM && steam_id_ == 0) continue;

        // Register the STEAM node every tick (even when idle/fuel-less) so pipes
        // can attach to a solid boiler that is not currently burning.
        if (energy.type == EnergyType::STEAM) {
            if (pipeClient_) {
                pipeClient_->publishNodeUpdate(
                    static_cast<uint64_t>(ent),
                    static_cast<int32_t>(machine.x),
                    static_cast<int32_t>(machine.y),
                    static_cast<int32_t>(machine.z),
                    energy.current, energy.capacity,
                    energy.maxInput, energy.maxOutput,
                    energy.tier, static_cast<int32_t>(energy.type),
                    true, false);
            }
            if (fluidClient_) {
                fluidClient_->publishNodeUpdate(
                    static_cast<uint64_t>(ent), machine.x, machine.y, machine.z,
                    steam_id_,                                // steam
                    energy.current, energy.capacity,
                    0, energy.maxOutput, energy.tier,
                    true, false);                           // is_source=true
            }
        }

        spdlog::debug("[GeneratorSystem] processing entity {} machine_id={} slots={} coal={} energy={}/{}",
                      static_cast<uint32_t>(ent), machine.machine_id,
                      container.slots.size(),
                      (!container.slots.empty() ? container.slots[0].item_id : 0),
                      energy.current, energy.capacity);
        // The burn rate and the buffer size are CONTENT, not derived from the
        // fuel: SimulationEngine.cpp:241-244 copies MachineInfo::capacity /
        // ::maxInput / ::maxOutput out of machines.yaml into the very
        // EnergyStorage this tick is holding. They are never 0 for a
        // well-configured generator, so there is no honest fallback number —
        // 32/10000 were invented here, and a capacity invented this way is
        // published to the client as fact.
        //
        // This gate sits ABOVE the isFull() check and above the fuel scan, both
        // deliberately. Above isFull() because a generator with capacity 0 is
        // trivially `current >= 0` — i.e. permanently "full" — so checking later
        // would leave a capacity-0 generator silently idle forever, which is the
        // same invisibility this removes, just quieter. Above the fuel scan
        // because burning coal into a buffer that can never charge destroys the
        // item for nothing.
        //
        // Fail LOUD, then skip: log and leave the fuel untouched. Same
        // fail-closed shape as the steam-id gate at the top of the tick.
        if (energy.capacity <= 0 || energy.maxOutput <= 0) {
            const MachineInfo* minfo = MachineRegistry::instance()
                                           ? MachineRegistry::instance()->Get(machine.machine_id)
                                           : nullptr;
            spdlog::error(
                "[GeneratorSystem] machine {} has no usable energy data "
                "(registry entry: {}, capacity={}, max_output={}) — refusing "
                "to burn fuel. Declare energy.capacity and energy.max_output "
                "in machines.yaml.",
                machine.machine_id,
                minfo ? "present" : "MISSING",
                minfo ? minfo->capacity : energy.capacity,
                minfo ? minfo->maxOutput : energy.maxOutput);
            continue;
        }

        if (energy.isFull()) continue;

        int32_t& remaining = burnEnergy_[ent];
        if (remaining <= 0) {
            for (auto& slot : container.slots) {
                if (slot.count == 0) continue;
                auto it = FuelValues().find(slot.item_id);
                if (it != FuelValues().end()) {
                    slot.count--;
                    remaining = it->second;
                    burnFuel_[ent] = slot.item_id;
                    break;
                }
            }
            if (remaining <= 0) continue;
        }

        // Rate is the component's own max_output — the value seeded from
        // machines.yaml, the same source the block-entity update publishes at
        // the bottom of this tick, so rate and capacity can no longer disagree.
        int32_t produced = std::min(energy.maxOutput, remaining);
        int32_t accepted = energy.produceEnergy(produced);
        remaining -= accepted;

        if (pipeClient_ && energy.type == EnergyType::HEAT) {
            pipeClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent),          // node_id = ECS entity id
                static_cast<int32_t>(machine.x),
                static_cast<int32_t>(machine.y),
                static_cast<int32_t>(machine.z),
                energy.current,
                energy.capacity,
                energy.maxInput,
                energy.maxOutput,
                energy.tier,
                static_cast<int32_t>(energy.type),
                true,   // is_source (generator produces)
                false   // is_sink
            );
        }

        // ELECTRICITY generators also publish node update for CableGraph registration
        if (pipeClient_ && energy.type == EnergyType::ELECTRICITY) {
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

        // STEAM generators (solid boiler) publish node update for PipeNetwork registration
        if (pipeClient_ && energy.type == EnergyType::STEAM) {
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

            // Steam fluid source publish: only STEAM generators (solid boiler)
            // expose a steam node. Heat generators must not register as a steam
            // source, or a consumer's BFS would drain phantom steam from them.
            if (fluidClient_) {
                fluidClient_->publishNodeUpdate(
                    static_cast<uint64_t>(ent), machine.x, machine.y, machine.z,
                    steam_id_,                                // steam
                    energy.current, energy.capacity,
                    0, energy.maxOutput, energy.tier,
                    true, false);                           // is_source=true
            }
        }

        if (remaining <= 0) {
            burnEnergy_.erase(ent);
            burnFuel_.erase(ent);
        }

        float pct = 0.0f;
        {
            auto it = FuelValues().find(container.slots.empty() ? 0 : container.slots[0].item_id);
            if (it != FuelValues().end() && it->second > 0) {
                pct = 1.0f - static_cast<float>(remaining) / static_cast<float>(it->second);
            }
        }
        std::vector<uint8_t> invData(container.slots.size() * 5);
        {
            uint8_t* ptr = invData.data();
            for (const auto& s : container.slots) {
                std::memcpy(ptr, &s.item_id, sizeof(uint16_t)); ptr += sizeof(uint16_t);
                *ptr++ = s.count;
                std::memcpy(ptr, &s.meta, sizeof(uint16_t)); ptr += sizeof(uint16_t);
            }
        }
        int slotsIn = static_cast<int>(container.slot_count);
        if (auto* info = MachineRegistry::instance()->Get(machine.machine_id)) {
            slotsIn = info->slots_in;
        }
        float heatRatio = 0.0f;
        if (auto* hic = reg_.try_get<HeatIntakeComponent>(ent)) {
            heatRatio = hic->ratio();
        }
        double steamCur = -1.0, steamCap = -1.0;
        if (auto* soc = reg_.try_get<SteamOutputComponent>(ent)) {
            steamCur = soc->steam_stored;
            steamCap = soc->steam_capacity;
        }
        events_->publishBlockEntityUpdate(
            machine.x, machine.y, machine.z,
            machine.machine_id,
            invData,
            pct,
            static_cast<uint32_t>(energy.current),
            energy.type,
            static_cast<uint32_t>(energy.capacity),
            slotsIn,
            heatRatio,
            nullptr, steamCur, steamCap);
    }
}

} // namespace simcore
