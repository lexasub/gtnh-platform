#pragma once
#include "IActionHandler.h"
#include <functional>
#include <memory>
#include <vector>

#include <entt/entt.hpp>
#include "core_generated.h"

namespace simcore {

class IBlockRepository;
class IEventPublisher;
class SimulationEngine;
class PlayerInventoryStore;
class EntityStateStoreClient;

using ItemGiveCallback = std::function<void(
    uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot)>;
using DrillUseCallback = std::function<void(
    uint64_t player_id, int32_t x, int32_t y, int32_t z, uint16_t block_id)>;
using BlockPlacedCallback = std::function<void(
    uint64_t player_id, int32_t x, int32_t y, int32_t z, uint16_t block_id)>;
using PostCallback = std::function<void(std::function<void()>)>;

class SetBlockCASHandler : public IActionHandler {
public:
  SetBlockCASHandler(std::shared_ptr<IBlockRepository> repo,
                     std::shared_ptr<IEventPublisher> publisher,
                     std::shared_ptr<SimulationEngine> engine,
                     std::shared_ptr<PlayerInventoryStore> inventoryStore = nullptr,
                     ItemGiveCallback onGiveItem = nullptr,
                     DrillUseCallback onDrillUse = nullptr,
                     BlockPlacedCallback onBlockPlaced = nullptr,
                     PostCallback postToMain = nullptr);

  void handle(const void *table) override;

  void setEntityStateStore(std::shared_ptr<EntityStateStoreClient> client) {
    entityStateClient_ = std::move(client);
  }

private:
  auto handle(const Protocol::SetBlockAction *action) -> void;

  // ── Machine interaction helpers (right-click on a machine) ───────────────
  // Machines may predate this simcore instance: the block persists in
  // ChunkStore but no ECS entity exists (entities are created only on
  // block-change events). An entity-less machine is invisible to the tick
  // systems, so right-click lazily creates it and reports real energy state.
  entt::entity findEntityAt(int32_t x, int32_t y, int32_t z) const;
  void handleMachineInteraction(int32_t x, int32_t y, int32_t z,
                                uint16_t machine_id, uint64_t player_id,
                                uint32_t request_id);
  void publishMachineState(int32_t x, int32_t y, int32_t z,
                           uint16_t machine_id, uint64_t player_id,
                           uint32_t request_id);

  std::shared_ptr<IBlockRepository> repo_;
  std::shared_ptr<IEventPublisher> publisher_;
  std::shared_ptr<SimulationEngine> engine_;
  std::shared_ptr<PlayerInventoryStore> inventoryStore_;
  std::shared_ptr<EntityStateStoreClient> entityStateClient_;
  ItemGiveCallback onGiveItem_;
  DrillUseCallback onDrillUse_;
  BlockPlacedCallback onBlockPlaced_;
  PostCallback postToMain_;

  static constexpr uint16_t kChestEntityType = 3;
  void publishChestState(int32_t x, int32_t y, int32_t z, uint16_t chest_id, uint64_t player_id, uint32_t request_id);
};

} // namespace simcore