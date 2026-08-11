#pragma once
#include <cstdint>
#include <common/ItemId.h>
namespace simcore {
namespace HeatConstants {
constexpr float OVERHEAT_WARNING_THRESHOLD = 0.90f;
constexpr float OVERHEAT_CRITICAL_THRESHOLD = 1.00f;
constexpr float ENVIRONMENT_COOLING_RATE = 4.0f;
constexpr float WATER_COOLING_MULTIPLIER = 3.0f;
constexpr uint32_t EXPLOSION_DELAY_TICKS = 60;
constexpr uint32_t COOLANT_COOLING_AMOUNT = 50;
constexpr uint16_t COOLANT_ITEM_ID = ItemId::pack("0:11111:4");
constexpr int32_t CONVERSION_RATE = 1;
// HEAT sink keep-filled target for pipe-fed boilers: the boiler pulls from the
// pipe network (EnergyConsumeReq) to keep heat_stored near this level. At 20 Hz
// sim ticks and CONVERSION_RATE=1, 100 units ≈ 5 s of conversion headroom.
constexpr int32_t HEAT_SINK_REPLENISH_TARGET = 100;
} // namespace HeatConstants
} // namespace simcore
