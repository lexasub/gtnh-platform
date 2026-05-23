#include "WrenchHandler.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/SideConfig.h"
#include "ECS/components/Position.h"
#include <entt/entt.hpp>
#include <spdlog/spdlog.h>

namespace simcore {

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
    
    result.success = true;
    result.error = "";
    result.newRole = newRole;
    for (int i = 0; i < 6; i++) result.allRoles[i] = machine.side_config[i];
    
    spdlog::info("[Wrench] Face {} at ({},{},{}) cycled role {}→{}", face, x, y, z, currentRole, newRole);
    return result;
}

} // namespace simcore
