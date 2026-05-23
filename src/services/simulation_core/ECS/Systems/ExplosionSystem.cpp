#include "ExplosionSystem.h"
#include <spdlog/spdlog.h>

namespace simcore {

void ExplosionSystem::tick(float /*dt*/) {
    auto view = reg_.view<MachineComponent, Position, OverheatComponent>();
    std::vector<entt::entity> toDestroy;

    for (auto ent : view) {
        auto& overheat = view.get<OverheatComponent>(ent);
        auto& pos = view.get<Position>(ent);

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
