#pragma once
#include "MachineRegistry.h"
#include "RecipeTypes.h"
#include <cstdint>
#include <array>
#include <vector>

struct HatchUpdateData {
  int32_t world_x, world_y, world_z;
  uint8_t hatch_type;  // matches Protocol::HatchType values
  uint8_t tier = 1;
  // Item slots in this hatch (packed bytes item_id(2)+count(1)+meta(2) per slot)
  std::vector<uint8_t> slot_data;
};

// Forward declare Protocol types if needed, but we can keep status as integer.
// For simplicity, we include the generated header in cpp only.
namespace simcore {

class IEventPublisher {
public:
  virtual ~IEventPublisher() = default;

  // status: 0=COMMITTED, 1=REJECTED, 2=CONFLICT (matching
  // Protocol::BlockAckStatus)
  virtual void publishBlockAck(uint8_t status, int32_t x, int32_t y, int32_t z,
                               uint16_t block_id, uint8_t meta,
                               const char *reason,
                               uint32_t request_id = 0,
                               uint8_t action_type = 1  // RIGHT_MOUSE_CLICK
                               ) = 0;

  // Server-authoritative UI/effect instruction for a block action
  // (Protocol::BlockDirective: NONE/OPEN_UI/PLAY_ANIMATION).
  virtual void publishBlockDirective(uint8_t directive, uint16_t block_id,
                                     int32_t x, int32_t y, int32_t z,
                                     uint32_t request_id = 0,
                                     uint8_t action_type = 1) = 0;

  virtual void publishBlockChangedEvent(int32_t x, int32_t y, int32_t z,
                                        uint16_t block_id, uint8_t meta,
                                        uint32_t request_id = 0,
                                        uint64_t source_player_id = 0) = 0;

  // Machine progress/inventory update: published each tick for entities with
  // block entities (machines, workbenches, etc.). Clients use this to
  // render progress bars, inventory UI, and energy indicators.
  virtual void publishBlockEntityUpdate(
      int32_t x, int32_t y, int32_t z, uint16_t machine_type,
      const std::vector<uint8_t> &inventory_data, float progress,
      uint32_t energy, EnergyType energy_type = EnergyType::ELECTRICITY,
      uint32_t energy_capacity = 0, int slots_in = -1,
      float heat_ratio = 0.0f,
      const std::vector<HatchUpdateData>* hatches = nullptr) = 0;

  virtual void publishMachineSlotResponse(int32_t x, int32_t y, int32_t z,
                                          uint16_t slot_idx, bool success,
                                          uint16_t item_id, uint8_t count,
                                          uint16_t meta,
                                          const char *error = nullptr) = 0;

  // Machine side config update: published when wrench cycles a face role
  virtual void publishMachineConfigUpdatedEvent(int32_t x, int32_t y, int32_t z,
                                                const std::array<uint8_t, 6> &side_config) = 0;

  virtual void publishMultiblockCreated(uint64_t controller_id, int32_t x,
                                         int32_t y, int32_t z,
                                         uint16_t mb_type) = 0;
  virtual void publishMultiblockDestroyed(uint64_t controller_id) = 0;

  // Workbench grid state: published when a player opens a workbench (loads
  // saved state) and after each craft (saves new state).
  virtual void publishGridUpdate(int32_t x, int32_t y, int32_t z,
                                const std::vector<RecipeManager::ItemStack> &grid) = 0;
};

} // namespace simcore
