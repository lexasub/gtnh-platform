#pragma once
#include <cstdint>
#include <vector>
#include "MachineRegistry.h"

// Forward declare Protocol types if needed, but we can keep status as integer.
// For simplicity, we include the generated header in cpp only.
namespace simcore {

    class IEventPublisher {
    public:
        virtual ~IEventPublisher() = default;

        // status: 0=COMMITTED, 1=REJECTED, 2=CONFLICT (matching Protocol::BlockAckStatus)
        virtual void publishBlockAck(uint8_t status,
                                     int32_t x, int32_t y, int32_t z,
                                     uint16_t block_id, uint8_t meta,
                                     const char* reason) = 0;

        virtual void publishBlockChangedEvent(int32_t x, int32_t y, int32_t z,
                                              uint16_t block_id, uint8_t meta) = 0;

        // Machine progress/inventory update: published each tick for entities with
        // block entities (machines, workbenches, etc.). Clients use this to
        // render progress bars, inventory UI, and energy indicators.
        virtual void publishBlockEntityUpdate(int32_t x, int32_t y, int32_t z,
                                              uint16_t machine_type,
                                              const std::vector<uint8_t>& inventory_data,
                                              float progress,
                                              uint32_t energy,
                                              EnergyType energy_type = EnergyType::ELECTRICITY,
                                              uint32_t energy_capacity = 0,
                                              int slots_in = -1) = 0;

        virtual void publishMachineSlotResponse(
            int32_t x, int32_t y, int32_t z,
            uint16_t slot_idx,
            bool success,
            uint16_t item_id,
            uint8_t count,
            uint16_t meta,
            const char* error = nullptr) = 0;
    };

} // namespace simcore
