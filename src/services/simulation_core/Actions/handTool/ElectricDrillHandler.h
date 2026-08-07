#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Storage/PlayerInventoryStore.h"

namespace simcore {

struct DrillMineResult {
  bool success = false;
  std::string error;
  uint32_t energy_remaining = 0;
  uint16_t mined_block_id = 0;
};

class ElectricDrillHandler {
public:
  ElectricDrillHandler(
      std::function<uint16_t(int32_t, int32_t, int32_t)> getBlock,
      std::function<void(int32_t, int32_t, int32_t, uint16_t)> setBlock,
      std::shared_ptr<PlayerInventoryStore> inventoryStore);

  DrillMineResult mineBlock(uint64_t player_id, int32_t x, int32_t y, int32_t z,
                            uint16_t tool_item_id, uint8_t slot_idx);

  static bool canMineBlock(uint16_t tool_item_id, uint16_t block_id);
  static float getMiningTicks(uint16_t tool_item_id, uint16_t block_id);

private:
  std::function<uint16_t(int32_t, int32_t, int32_t)> getBlock_;
  std::function<void(int32_t, int32_t, int32_t, uint16_t)> setBlock_;
  std::shared_ptr<PlayerInventoryStore> inventoryStore_;
};

} // namespace simcore
