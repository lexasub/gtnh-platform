#include "BoilerSystem.h"
#include "HeatConstants.h"
#include "Network/FluidClient.h"
#include <common/ItemId.h>
#include <common/ResourcePortClient.h>
#include <spdlog/spdlog.h>
#include "../components/HeatIntakeComponent.h"
#include "../components/EnergyStorage.h"
#include "../components/SteamOutputComponent.h"

namespace simcore {

BoilerSystem::BoilerSystem(entt::registry& reg,
                           std::shared_ptr<IEventPublisher> events,
                           std::shared_ptr<PipeEnergyClient> pipeClient,
                           std::shared_ptr<FluidClient> fluidClient,
                           std::shared_ptr<gtnh::common::IResourcePortClient> portClient,
                           std::uint16_t steam_item_id,
                           std::shared_ptr<ResourceBufferStatePublisher> statePublisher)
    : reg_(reg), events_(events), pipeClient_(pipeClient), fluidClient_(fluidClient),
      portClient_(portClient), statePublisher_(statePublisher), steam_id_(steam_item_id)
{
}

void BoilerSystem::tick(float /*dt*/) {
    // ── Steam heat boiler (1110:011:1): convert neighbour HEAT → STEAM ──
    // Heat arrives via AdjacencyTransferSystem into HeatIntakeComponent.heat_stored
    // (which keeps EnergyStorage.current in sync for HEAT-type machines). BoilerSystem
    // consumes both synced fields and stores produced STEAM in SteamOutputComponent.
    auto heatView = reg_.view<MachineComponent, EnergyStorage, HeatIntakeComponent, SteamOutputComponent>();
    for (auto ent : heatView) {
        auto& machine = heatView.get<MachineComponent>(ent);
        if (machine.machine_id != ItemId::pack("1110:011:1")) continue;
        auto& energy = heatView.get<EnergyStorage>(ent);
        auto& heatIntake = heatView.get<HeatIntakeComponent>(ent);
        auto& steam = heatView.get<SteamOutputComponent>(ent);

        // Register/refresh pipe nodes every tick — a cold or steam-full boiler
        // must still exist in the pipe network, or pipes can never attach to it.
        // Fail-closed: without a resolved Steam id the produced steam could
        // never be identified or drained, so no heat is converted.
        int32_t maxOut = 0;
        // A boiler's HEAT node is populated by AdjacencyTransferSystem before
        // this system runs; keep the component mirror authoritative for the
        // conversion even when a legacy node update is delayed.
        if (auto* current_energy = reg_.try_get<EnergyStorage>(ent)) {
            heatIntake.heat_stored = std::max(heatIntake.heat_stored, current_energy->current);
        }
        if (steam_id_ != 0 && heatIntake.heat_stored > 0 &&
            steam.steam_stored < steam.steam_capacity) {
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
        if (fluidClient_ && steam_id_ != 0) {
            fluidClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent), machine.x, machine.y, machine.z,
                steam_id_,                               // steam fluid id
                static_cast<int32_t>(steam.steam_stored),
                static_cast<int32_t>(steam.steam_capacity),
                0, maxOut, energy.tier,
                true, false);                           // is_source=true, is_sink=false
        }

        // Typed resource ports (openspec refactor-fluid-port-accounting 2.4):
        // the boiler is a converter, so it registers a separate HU sink and a
        // separate FLUID steam source on the same owner — never one node
        // identity for both domains. Republished every tick at the same epoch;
        // receivers dedupe on (owner, kind, port, epoch). Each record is
        // self-contained, so publication order does not matter. Legacy node
        // updates above stay until the typed path is end-to-end (removal is a
        // later task).
        const std::uint64_t owner = static_cast<std::uint64_t>(ent);
        const std::int32_t px = static_cast<std::int32_t>(machine.x);
        const std::int32_t py = static_cast<std::int32_t>(machine.y);
        const std::int32_t pz = static_cast<std::int32_t>(machine.z);
        if (statePublisher_) {
            statePublisher_->PublishBufferState(
                owner, BoilerPorts::kBoilerHuSinkPortId,
                gtnh::common::ResourceKind::HU, 0,
                heatIntake.heat_stored, heatIntake.heat_capacity,
                HeatConstants::HEAT_SINK_REPLENISH_TARGET,
                port_epochs_.EpochOf(owner, BoilerPorts::kBoilerHuSinkPortId),
                px, py, pz);
            if (steam_id_ != 0) {
                statePublisher_->PublishBufferState(
                    owner, BoilerPorts::kBoilerSteamSourcePortId,
                    gtnh::common::ResourceKind::FLUID, steam_id_,
                    static_cast<std::int32_t>(steam.steam_stored),
                    static_cast<std::int32_t>(steam.steam_capacity),
                    HeatConstants::CONVERSION_RATE,
                    port_epochs_.EpochOf(owner, BoilerPorts::kBoilerSteamSourcePortId),
                    px, py, pz);
            }
        }

        if (portClient_) {
            const std::uint64_t owner = static_cast<std::uint64_t>(ent);
            const std::int32_t px = static_cast<std::int32_t>(machine.x);
            const std::int32_t py = static_cast<std::int32_t>(machine.y);
            const std::int32_t pz = static_cast<std::int32_t>(machine.z);

            const gtnh::common::ResourcePort hu_sink = BoilerPorts::MakeHuSinkPort(
                owner, px, py, pz,
                heatIntake.heat_capacity,
                HeatConstants::HEAT_SINK_REPLENISH_TARGET,
                port_epochs_.EpochOf(owner, BoilerPorts::kBoilerHuSinkPortId));
            portClient_->PublishPortRegister(hu_sink, 0);  // HU: no resource id
            if (steam_id_ != 0) {
                const gtnh::common::ResourcePort steam_source =
                    BoilerPorts::MakeSteamSourcePort(
                        owner, px, py, pz,
                        static_cast<std::int32_t>(steam.steam_capacity),
                        HeatConstants::CONVERSION_RATE,
                        port_epochs_.EpochOf(owner, BoilerPorts::kBoilerSteamSourcePortId));
                portClient_->PublishPortRegister(steam_source, steam_id_);
            }
        }
    }
}

std::uint64_t BoilerSystem::replacePort(std::uint64_t owner_id,
                                        gtnh::common::PortId port_id) {
    return port_epochs_.Replace(owner_id, port_id);
}

} // namespace simcore
