#include "BoilerSystem.h"
#include "HeatConstants.h"
#include "Network/FluidClient.h"
#include <common/ItemId.h>
#include <spdlog/spdlog.h>
#include "../components/EnergyStorage.h"
#include "../components/HeatIntakeComponent.h"
#include "../components/InventoryContainer.h"
#include "../components/SteamOutputComponent.h"

namespace simcore {

namespace {

// Solid fuel burn values for the solid-fuel boiler (steam units per item).
const std::unordered_map<uint16_t, int32_t> kSolidFuelEnergy = {
    {ItemId::pack("0:11110:2"), 8000}, // coal
    {ItemId::pack("0:10:00:0"), 2000}, // oak planks
    {ItemId::pack("0:11110:0"), 500},  // stick
};

} // namespace

BoilerSystem::BoilerSystem(entt::registry& reg,
                           std::shared_ptr<IEventPublisher> events,
                           std::shared_ptr<PipeEnergyClient> pipeClient,
                           std::shared_ptr<FluidClient> fluidClient)
    : reg_(reg), events_(events), pipeClient_(pipeClient), fluidClient_(fluidClient)
{
}

void BoilerSystem::tick(float /*dt*/) {
    // ── Steam heat boiler (1110:01:1): convert neighbour HEAT → STEAM ──
    // Heat arrives via AdjacencyTransferSystem into HeatIntakeComponent.heat_stored
    // (which keeps EnergyStorage.current in sync for HEAT-type machines). BoilerSystem
    // consumes both synced fields and stores produced STEAM in SteamOutputComponent.
    auto heatView = reg_.view<MachineComponent, EnergyStorage, HeatIntakeComponent, SteamOutputComponent>();
    for (auto ent : heatView) {
        auto& machine = heatView.get<MachineComponent>(ent);
        if (machine.machine_id != ItemId::pack("1110:01:1")) continue;
        auto& energy = heatView.get<EnergyStorage>(ent);
        auto& heatIntake = heatView.get<HeatIntakeComponent>(ent);
        auto& steam = heatView.get<SteamOutputComponent>(ent);

        // Register/refresh pipe nodes every tick — a cold or steam-full boiler
        // must still exist in the pipe network, or pipes can never attach to it.
        int32_t maxOut = 0;
        if (heatIntake.heat_stored > 0 && steam.steam_stored < steam.steam_capacity) {
            double toConvert = std::min({
                static_cast<double>(HeatConstants::CONVERSION_RATE),
                static_cast<double>(heatIntake.heat_stored),
                steam.steam_capacity - steam.steam_stored
            });
            heatIntake.heat_stored -= toConvert;
            energy.current -= toConvert;
            steam.steam_stored += toConvert;
            maxOut = static_cast<int32_t>(toConvert);

            spdlog::debug("Heat boiler {} at entity {} produced {} STEAM from {} HEAT",
                          machine.machine_id, static_cast<uint32_t>(ent),
                          toConvert, static_cast<uint32_t>(toConvert));
        }

        // Publish UI state every tick: a cold or steam-full boiler still reports
        // its levels, or the client window hides the SU bar / flags stale state.
        events_->publishBlockEntityUpdate(
            machine.x, machine.y, machine.z, machine.machine_id,
            {}, 0.0f,
            static_cast<uint32_t>(heatIntake.heat_stored),
            EnergyType::HEAT, 0, -1,
            heatIntake.ratio(), {},
            steam.steam_stored, steam.steam_capacity);

        if (pipeClient_) {
            pipeClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent),
                machine.x, machine.y, machine.z,
                static_cast<int32_t>(steam.steam_stored),
                static_cast<int32_t>(steam.steam_capacity),
                0, maxOut,
                energy.tier, static_cast<int32_t>(EnergyType::STEAM),
                true, false);

            // HEAT sink node: lets a heat_pipe network deliver HEAT into the
            // boiler (distributeHeat moves excess heat from sources to sinks).
            // Publish every tick so the node state stays fresh.
            pipeClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent),
                machine.x, machine.y, machine.z,
                energy.current, energy.capacity,
                0, 0,
                energy.tier, static_cast<int32_t>(EnergyType::HEAT),
                false, true);

            // Pull HEAT from the pipe network when the local buffer runs low:
            // the boiler itself is not a recipe machine, so MachineSystem's
            // energy-gated consume path never fires for it.
            if (heatIntake.heat_stored < HeatConstants::HEAT_SINK_REPLENISH_TARGET &&
                steam.steam_stored < steam.steam_capacity) {
                int32_t needed = HeatConstants::HEAT_SINK_REPLENISH_TARGET - heatIntake.heat_stored;
                pipeClient_->sendConsumeRequest(
                    static_cast<uint64_t>(ent),
                    machine.x, machine.y, machine.z,
                    static_cast<int32_t>(EnergyType::HEAT),
                    needed);
            }
        }
        if (fluidClient_) {
            fluidClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent), machine.x, machine.y, machine.z,
                ItemId::pack("1111:11:1"),              // steam fluid id
                static_cast<int32_t>(steam.steam_stored),
                static_cast<int32_t>(steam.steam_capacity),
                0, maxOut, energy.tier,
                true, false);                           // is_source=true, is_sink=false
        }
    }

    // ── Solid fuel boiler (1110:01:0): burn inventory fuel → STEAM ──
    // Unlike the heat boiler above, this variant makes its own heat from solid
    // fuel — no HeatIntakeComponent and no HEAT pipe input.
    auto solidView =
        reg_.view<MachineComponent, InventoryContainer, EnergyStorage, SteamOutputComponent>();
    for (auto ent : solidView) {
        auto &machine = solidView.get<MachineComponent>(ent);
        if (machine.machine_id != ItemId::pack("1110:01:0")) continue;
        auto &container = solidView.get<InventoryContainer>(ent);
        auto &energy = solidView.get<EnergyStorage>(ent);
        auto &steam = solidView.get<SteamOutputComponent>(ent);

        int32_t maxOut = 0;
        if (steam.steam_stored < steam.steam_capacity) {
            // Ignite a new fuel item when nothing is currently burning.
            auto pending = burnEnergy_.find(ent);
            if (pending == burnEnergy_.end()) {
                for (auto &slot : container.slots) {
                    if (slot.count == 0)
                        continue;
                    auto fuel = kSolidFuelEnergy.find(slot.item_id);
                    if (fuel == kSolidFuelEnergy.end())
                        continue;
                    --slot.count;
                    if (slot.count == 0) {
                        slot.item_id = 0;
                        slot.meta = 0;
                    }
                    burnEnergy_[ent] = fuel->second;
                    pending = burnEnergy_.find(ent);
                    break;
                }
            }
            if (pending != burnEnergy_.end()) {
                int32_t rate = energy.maxOutput > 0 ? energy.maxOutput : 32;
                double produced = std::min({static_cast<double>(rate),
                                            static_cast<double>(pending->second),
                                            steam.steam_capacity - steam.steam_stored});
                steam.steam_stored += produced;
                pending->second -= static_cast<int32_t>(produced);
                if (pending->second <= 0)
                    burnEnergy_.erase(pending);
                maxOut = static_cast<int32_t>(produced);

                spdlog::debug("Solid boiler {} at entity {} produced {} STEAM from fuel",
                              machine.machine_id, static_cast<uint32_t>(ent), produced);
            }
        }

        // Publish UI state every tick: a cold or steam-full boiler still reports
        // its levels, or the client window hides the SU bar / flags stale state.
        events_->publishBlockEntityUpdate(
            machine.x, machine.y, machine.z, machine.machine_id,
            {}, 0.0f,
            0u,
            EnergyType::STEAM, 0, -1,
            0.0f, {},
            steam.steam_stored, steam.steam_capacity);

        if (pipeClient_) {
            // STEAM source node: lets a steam_pipe network pull steam from the
            // boiler. Publish every tick so the node state stays fresh.
            pipeClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent),
                machine.x, machine.y, machine.z,
                static_cast<int32_t>(steam.steam_stored),
                static_cast<int32_t>(steam.steam_capacity),
                0, maxOut,
                energy.tier, static_cast<int32_t>(EnergyType::STEAM),
                true, false);

            // No HEAT sink node here: the solid boiler makes its own heat from
            // fuel and never receives heat from a pipe network.
        }
        if (fluidClient_) {
            fluidClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent), machine.x, machine.y, machine.z,
                ItemId::pack("1111:11:1"),              // steam fluid id
                static_cast<int32_t>(steam.steam_stored),
                static_cast<int32_t>(steam.steam_capacity),
                0, maxOut, energy.tier,
                true, false);                           // is_source=true, is_sink=false
        }
    }
}

} // namespace simcore
