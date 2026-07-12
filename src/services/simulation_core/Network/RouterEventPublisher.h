#pragma once
#include "IEventPublisher.h"
#include <memory>

namespace simcore {

class IoUringRouterClient;

class RouterEventPublisher : public IEventPublisher {
public:
  explicit RouterEventPublisher(std::shared_ptr<IoUringRouterClient> router);

  void publishBlockAck(uint8_t status, int32_t x, int32_t y, int32_t z,
                       uint16_t block_id, uint8_t meta,
                       const char *reason) override;

  void publishBlockChangedEvent(int32_t x, int32_t y, int32_t z,
                                uint16_t block_id, uint8_t meta) override;

  void publishBlockEntityUpdate(
      int32_t x, int32_t y, int32_t z, uint16_t machine_type,
      const std::vector<uint8_t> &inventory_data, float progress,
      uint32_t energy, EnergyType energy_type = EnergyType::ELECTRICITY,
      uint32_t energy_capacity = 0, int slots_in = -1,
      float heat_ratio = 0.0f) override;

  void publishMachineSlotResponse(int32_t x, int32_t y, int32_t z,
                                   uint16_t slot_idx, bool success,
                                   uint16_t item_id, uint8_t count,
                                   uint16_t meta,
                                   const char *error = nullptr) override;

  void publishMachineConfigUpdatedEvent(int32_t x, int32_t y, int32_t z,
                                        const std::array<uint8_t, 6> &side_config) override;

private:
  std::shared_ptr<IoUringRouterClient> router_;
};

} // namespace simcore
