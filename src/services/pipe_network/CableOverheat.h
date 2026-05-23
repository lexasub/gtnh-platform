#pragma once
#include <cstdint>
#include <algorithm>

constexpr float OVERHEAT_THRESHOLD = 100.0f;
constexpr float HEAT_SPIKE_PER_VOLT_OVER = 50.0f;
constexpr float HEAT_PER_EXTRA_PACKET = 1.0f;
constexpr float COOLDOWN_PER_TICK = 2.0f;

struct OverheatResult {
    bool exploded;
    float temperature;
};

inline OverheatResult calculateOverheat(uint32_t voltage, uint32_t maxVoltage,
                                         int32_t packetsThisTick, uint32_t ampacity,
                                         float currentTemp) {
    float temp = currentTemp;
    if (voltage > maxVoltage) {
        temp += static_cast<float>(voltage - maxVoltage) * HEAT_SPIKE_PER_VOLT_OVER;
    }
    if (packetsThisTick > static_cast<int32_t>(ampacity)) {
        temp += static_cast<float>(packetsThisTick - static_cast<int32_t>(ampacity)) * HEAT_PER_EXTRA_PACKET;
    }
    temp -= COOLDOWN_PER_TICK;
    temp = std::max(0.0f, temp);
    return OverheatResult{temp >= OVERHEAT_THRESHOLD, temp};
}
