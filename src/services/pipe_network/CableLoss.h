#pragma once
#include <algorithm>
#include <cstdint>

inline constexpr float cableEnergyLoss(float distance, float lossPerBlock) {
  return distance * lossPerBlock;
}

inline constexpr float effectiveVoltage(float distance, float lossPerBlock,
                                        float voltage) {
  float loss = cableEnergyLoss(distance, lossPerBlock);
  return std::max(0.0f, voltage - loss);
}
