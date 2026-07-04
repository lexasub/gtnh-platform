#pragma once
#include <cstdint>
#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ECS/components/InventoryContainer.h"

namespace simcore {
class EntityStateStoreClient;

class WorldContainerInventory {
public:
  WorldContainerInventory(entt::registry &reg,
                          std::shared_ptr<EntityStateStoreClient> storage);

  void onContainerOpen(uint64_t player_id, uint32_t x, uint32_t y, uint32_t z,
                       uint16_t entity_type);
  void onContainerClose(uint64_t player_id, uint32_t x, uint32_t y, uint32_t z);
  void onContainerAction(uint64_t player_id, uint32_t x, uint32_t y, uint32_t z,
                         uint8_t action, uint8_t src_slot, uint8_t dst_slot,
                         uint8_t count);
  InventoryContainer *getContainer(uint32_t x, uint32_t y, uint32_t z);

private:
  struct OpenContainer {
    entt::entity entity;
    uint64_t player_id;
  };

  entt::registry &reg_;
  std::shared_ptr<EntityStateStoreClient> storage_;
  std::unordered_map<uint64_t, OpenContainer> open_containers_;

  static uint64_t packKey(uint32_t x, uint32_t y, uint32_t z);

  void saveContainer(uint32_t x, uint32_t y, uint32_t z,
                     std::unordered_map<uint64_t, OpenContainer>::iterator it);
  void loadContainer(uint32_t x, uint32_t y, uint32_t z,
                     InventoryContainer container, uint64_t player_id,
                     uint64_t key);

  std::vector<uint8_t> serializeToBlob(const InventoryContainer &container);
  void deserializeFromBlob(const std::vector<uint8_t> &blob,
                           InventoryContainer &container);
};

} // namespace simcore
