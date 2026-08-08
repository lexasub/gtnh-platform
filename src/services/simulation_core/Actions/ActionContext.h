#pragma once
#include "Actions/Callbacks.h"
#include "core_generated.h"
#include "../../../data/registry/ToolIds.h"
#include <common/ItemId.h>
#include <cstdint>
#include <memory>

struct MachineInfo;

namespace simcore {

class IBlockRepository;
class IEventPublisher;
class SimulationEngine;
class PlayerInventoryStore;
class EntityStateStoreClient;

inline constexpr uint16_t kChestBlockId = ItemId::pack("0:10:11:0");

inline void faceAdjacentBlock(uint8_t face, int32_t& x, int32_t& y, int32_t& z) {
  switch (face) {
    case 0: --y; break; // DOWN
    case 1: ++y; break; // UP
    case 2: --z; break; // NORTH
    case 3: ++z; break; // SOUTH
    case 4: --x; break; // WEST
    case 5: ++x; break; // EAST
    default: break;
  }
}

inline bool isMiningTool(uint16_t item) {
  return item == ITEM_DRILL_ULV || item == ITEM_DRILL_LV ||
         item == ITEM_DRILL_MV || item == ITEM_DRILL_HV ||
         item == ITEM_CHAINSAW_LV;
}
// Immutable snapshot of one SetBlockAction plus every derived value the
// handlers need. All predicates are computed once in the constructor so
// handler canHandle()/handle() are pure reads.
class ActionContext {
public:
  ActionContext(const Protocol::SetBlockAction* action,
                std::shared_ptr<IBlockRepository> repo,
                std::shared_ptr<IEventPublisher> publisher,
                std::shared_ptr<SimulationEngine> engine,
                std::shared_ptr<PlayerInventoryStore> inventoryStore,
                std::shared_ptr<EntityStateStoreClient> entityStateClient,
                ItemGiveCallback onGiveItem, DrillUseCallback onDrillUse,
                BlockPlacedCallback onBlockPlaced, PostCallback postToMain);

  const Protocol::SetBlockAction* action = nullptr;

  uint8_t action_type = 0;
  int32_t x = 0, y = 0, z = 0;
  uint16_t expected_block_id = 0;
  uint16_t new_block_id = 0;
  uint64_t player_id = 0;
  uint32_t request_id = 0;
  uint16_t held_item = 0;
  uint8_t face = 0;

  // RIGHT click places on the face-adjacent cell against air; everything
  // else acts directly on (x,y,z) against the expected block.
  int32_t eff_x = 0, eff_y = 0, eff_z = 0;
  uint16_t eff_expected = 0;
  bool is_chest = false;
  const MachineInfo* machine_info = nullptr;

  // Deps live here as shared_ptr copies so async lambdas can capture them
  // safely regardless of which thread fires the callback.
  std::shared_ptr<IBlockRepository> repo_;
  std::shared_ptr<IEventPublisher> publisher_;
  std::shared_ptr<SimulationEngine> engine_;
  std::shared_ptr<PlayerInventoryStore> inventoryStore_;
  std::shared_ptr<EntityStateStoreClient> entityStateClient_;

  ItemGiveCallback onGiveItem_;
  DrillUseCallback onDrillUse_;
  BlockPlacedCallback onBlockPlaced_;
  PostCallback postToMain_;
};

} // namespace simcore
