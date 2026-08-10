#include "BoilerSystem.h"
#include "HeatConstants.h"
#include <common/ItemId.h>
#include <spdlog/spdlog.h>
#include "../components/HeatIntakeComponent.h"
#include "../components/EnergyStorage.h"
#include "../components/SteamOutputComponent.h"

namespace simcore {

BoilerSystem::BoilerSystem(entt::registry& reg,
                           std::shared_ptr<IEventPublisher> events,
                           std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), events_(events), pipeClient_(pipeClient)
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

        if (heatIntake.heat_stored <= 0) continue;
        if (steam.steam_stored >= steam.steam_capacity) continue;

        double toConvert = std::min({
            static_cast<double>(HeatConstants::CONVERSION_RATE),
            static_cast<double>(heatIntake.heat_stored),
            steam.steam_capacity - steam.steam_stored
        });
        heatIntake.heat_stored -= toConvert;
        energy.current -= toConvert;
        steam.steam_stored += toConvert;

        spdlog::debug("Heat boiler {} at entity {} produced {} STEAM from {} HEAT",
                      machine.machine_id, static_cast<uint32_t>(ent),
                      toConvert, static_cast<uint32_t>(toConvert));

        if (pipeClient_) {
            pipeClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent),
                machine.x, machine.y, machine.z,
                static_cast<int32_t>(steam.steam_stored),
                static_cast<int32_t>(steam.steam_capacity),
                0, static_cast<int32_t>(toConvert),
                energy.tier, static_cast<int32_t>(EnergyType::STEAM),
                true, false);
        }
        events_->publishBlockEntityUpdate(
            machine.x, machine.y, machine.z, machine.machine_id,
            {}, 0.0f,
            static_cast<uint32_t>(heatIntake.heat_stored),
            EnergyType::HEAT, 0, -1,
            heatIntake.ratio(), {},
            steam.steam_stored, steam.steam_capacity);
    }
}

} // namespace simcore
