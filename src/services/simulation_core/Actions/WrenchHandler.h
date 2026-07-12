#pragma once
#include <cstdint>
#include <entt/entt.hpp>
#include <string>

namespace simcore {

struct WrenchCycleResult {
  bool success;
  std::string error;
  uint8_t newRole;
  uint8_t allRoles[6];
};

class IEventPublisher;
class EntityStateStoreClient;

class WrenchHandler {
public:
  WrenchHandler(entt::registry &registry,
                std::shared_ptr<IEventPublisher> events,
                std::shared_ptr<EntityStateStoreClient> entityState);
  WrenchCycleResult cycleFace(uint64_t playerId, int32_t x, int32_t y,
                              int32_t z, uint8_t face);

  entt::entity findEntityAt(const entt::registry &reg, int32_t x, int32_t y, int32_t z);

private:
  entt::registry &m_registry;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<EntityStateStoreClient> entityState_;
};

} // namespace simcore
