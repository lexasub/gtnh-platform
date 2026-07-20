#include "WrenchHandler.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/SideConfig.h"
#include "ECS/components/Position.h"
#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <entt/entt.hpp>
#include <spdlog/spdlog.h>
#include <cstring>

#include "EntityStateStoreClient.h"
#include "Network/IEventPublisher.h"

namespace simcore {

WrenchHandler::WrenchHandler(entt::registry &registry,
                              std::shared_ptr<IEventPublisher> events,
                              std::shared_ptr<EntityStateStoreClient> entityState)
    : m_registry(registry), events_(std::move(events)), entityState_(std::move(entityState)) {}

WrenchCycleResult WrenchHandler::cycleFace(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint8_t face) {
    (void)playerId; // reserved for permission checks
    WrenchCycleResult result{false, "no_machine_at_position", 0, {}};
    
    auto view = m_registry.view<const Position, MachineComponent>();
    entt::entity found = entt::null;
    for (auto entity : view) {
        auto& pos = view.get<const Position>(entity);
        if (static_cast<int32_t>(pos.x) == x && static_cast<int32_t>(pos.y) == y && static_cast<int32_t>(pos.z) == z) {
            found = entity;
            break;
        }
    }
    
    if (found == entt::null) {
        return result;
    }
    
    auto& machine = m_registry.get<MachineComponent>(found);
    if (face > 5) {
        result.error = "invalid_face";
        return result;
    }
    
    bool hasFluid = false;
    bool hasEnergy = true;
    uint8_t currentRole = machine.side_config[face];
    uint8_t newRole = nextSideRole(currentRole, hasFluid, hasEnergy);
    
    machine.side_config[face] = newRole;
    
    // Persist side_config to EntityStateStore
    {
        flatbuffers::FlatBufferBuilder builder(128);
        std::vector<uint8_t> faces(machine.side_config, machine.side_config + 6);
        auto config = Protocol::CreateMachineConfigUpdatedDirect(
            builder, nullptr, machine.machine_id, 0, &faces, face, currentRole, newRole);
        builder.Finish(config);
        std::vector<uint8_t> eventData(builder.GetBufferPointer(),
                                        builder.GetBufferPointer() + builder.GetSize());

        entityState_->SaveEntityState(0, x, y, z, machine.machine_id,
                                     eventData, [this, x, y, z](bool success) {
            if (success) {
                spdlog::debug("[Wrench] Persisted side_config at ({},{},{})", x, y, z);
            } else {
                spdlog::warn("[Wrench] Failed to persist side_config at ({},{},{})", x, y, z);
            }
        });

        std::array<uint8_t, 6> sideConfigArr;
        std::memcpy(sideConfigArr.data(), machine.side_config, 6);
        events_->publishMachineConfigUpdatedEvent(x, y, z, sideConfigArr);
    }
    
    result.success = true;
    result.error = "";
    result.newRole = newRole;
    for (int i = 0; i < 6; i++) result.allRoles[i] = machine.side_config[i];
    
    spdlog::info("[Wrench] Face {} at ({},{},{}) cycled role {}→{}", face, x, y, z, currentRole, newRole);
    return result;
}

entt::entity WrenchHandler::findEntityAt(const entt::registry& reg, int32_t x, int32_t y, int32_t z) {
    auto view = reg.view<const Position>();
    for (auto entity : view) {
        auto& pos = view.get<const Position>(entity);
        if (static_cast<int32_t>(pos.x) == x && static_cast<int32_t>(pos.y) == y && static_cast<int32_t>(pos.z) == z) {
            return entity;
        }
    }
    return entt::null;
}

} // namespace simcore
