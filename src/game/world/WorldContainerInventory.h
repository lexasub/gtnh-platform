#pragma once
#include <cstdint>
#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>
#include <vector>

#include <engine/sim/components/InventoryContainer.h>

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
  // Destroys the container's ECS entity and drops its open_containers_ entry.
  // Only called once the contents are safely persisted (or when there is no
  // storage client to persist them to).
  void releaseContainer(std::unordered_map<uint64_t, OpenContainer>::iterator it);
  void loadContainer(uint32_t x, uint32_t y, uint32_t z,
                     InventoryContainer container, uint64_t player_id,
                     uint64_t key);
  // Registers `container` as an open container at (x,y,z) for `player_id`:
  // creates the ECS entity and populates open_containers_. Both the
  // saved-state path and the no-saved-state path funnel through here, so an
  // empty container is opened exactly like a hydrated one.
  void openContainer(uint32_t x, uint32_t y, uint32_t z,
                     InventoryContainer container, uint64_t player_id,
                     uint64_t key);
  // Removes `count` items from slot `index`, writing an EMPTY slot in place
  // instead of erasing it: a container slot index is a wire address, so the
  // slot vector must never shift under a client that addressed it by index.
  static void drainSlot(InventoryContainer &container, uint16_t index,
                        uint8_t count);

  std::vector<uint8_t> serializeToBlob(const InventoryContainer &container);
  void deserializeFromBlob(const std::vector<uint8_t> &blob,
                           InventoryContainer &container);
};

} // namespace simcore
