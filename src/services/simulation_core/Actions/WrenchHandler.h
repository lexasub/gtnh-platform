#pragma once
#include <cstdint>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include "ECS/components/MultiblockController.h"

namespace simcore {

struct WrenchCycleResult {
  bool success;
  std::string error;
  uint8_t newRole;
  uint8_t allRoles[6];
  uint16_t machine_id = 0; // packed block id of the cycled machine; 0 for hatches
};

class IEventPublisher;
class EntityStateStoreClient;

class WrenchHandler {
public:
  WrenchHandler(entt::registry &registry,
                std::shared_ptr<IEventPublisher> events,
                std::shared_ptr<EntityStateStoreClient> entityState,
                std::unordered_map<uint64_t, MultiblockController> *controllers = nullptr);
  WrenchCycleResult cycleFace(uint64_t playerId, int32_t x, int32_t y,
                              int32_t z, uint8_t face);

  entt::entity findEntityAt(const entt::registry &reg, int32_t x, int32_t y, int32_t z);

private:
  entt::registry &m_registry;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<EntityStateStoreClient> entityState_;
  std::unordered_map<uint64_t, MultiblockController> *controllers_ = nullptr;
};

} // namespace simcore
