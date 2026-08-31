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
// Boiler debits: produced STEAM per tick is min(CONVERSION_RATE, heat_stored,
// steam room). 100 @ 20 Hz = 2000 STEAM/s — enough to fill a pipe web and
// reach a steam consumer within a couple seconds instead of tens of seconds.
constexpr int32_t CONVERSION_RATE = 100;
// HEAT sink keep-filled target for pipe-fed boilers: the boiler pulls from the
// pipe network (EnergyConsumeReq) to keep heat_stored near this level so the
// debited STEAM is never starved by a tiny heat pool. 1000 @ 20 Hz ≈ 0.5 s of
// conversion headroom between pipe refills.
constexpr int32_t HEAT_SINK_REPLENISH_TARGET = 1000;
} // namespace HeatConstants
} // namespace simcore
