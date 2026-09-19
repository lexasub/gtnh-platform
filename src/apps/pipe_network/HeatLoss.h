#pragma once
#include <algorithm>
#include <cstdint>

namespace pipenet {

// Heat pipe tuning defaults. `resistance` on a PipeEdge is the per-block loss
// factor, mirroring `CableDef::lossPerBlock`.
inline constexpr float HEAT_PIPE_RESISTANCE = 0.02f;  // default loss factor per block
inline constexpr float HEAT_COOLDOWN_PER_TICK = 2.0f; // natural node cooling
inline constexpr float MAX_NODE_TEMPERATURE = 100.0f; // pipe node overheat threshold
inline constexpr float TEMPERATURE_PER_HEAT = 1.0f;   // temp rise per unit heat moved

// Mirrors CableLoss::cableEnergyLoss(): heat dissipated while traversing an
// edge = traversed distance x per-block resistance.
inline constexpr float heatTransferLoss(float distance, float resistancePerBlock) {
  return distance * resistancePerBlock;
}

// Mirrors CableLoss::effectiveVoltage(): heat remaining after edge loss,
// clamped at zero.
inline constexpr float effectiveHeatTransfer(float distance, float resistancePerBlock,
                                             float heat) {
  float loss = heatTransferLoss(distance, resistancePerBlock);
  return std::max(0.0f, heat - loss);
}

// Result of applying a cumulative traversal loss to a heat batch.
struct HeatLossResult {
  float effectiveHeat; // heat that survives the traversal
  float lostHeat;      // heat dissipated along the traversed edges
};

// Applies a total path loss (sum of resistance x distance over traversed
// edges) to a heat amount. Lost heat never exceeds the available heat.
inline HeatLossResult applyHeatLoss(float heat, float totalLoss) {
  float lost = std::min(heat, std::max(0.0f, totalLoss));
  return HeatLossResult{heat - lost, lost};
}

// Mirrors CableOverheat::calculateOverheat(): per-node temperature driven by
// heat throughput, cooled by a per-tick cooldown, clamped at zero.
struct NodeTemperatureResult {
  bool overheated;
  float temperature;
};

inline NodeTemperatureResult calculateNodeTemperature(
    float currentTemp, float heatThroughput,
    float maxTemperature = MAX_NODE_TEMPERATURE,
    float cooldownPerTick = HEAT_COOLDOWN_PER_TICK) {
  float temp = currentTemp + heatThroughput * TEMPERATURE_PER_HEAT;
  temp -= cooldownPerTick;
  temp = std::max(0.0f, temp);
  return NodeTemperatureResult{temp >= maxTemperature, temp};
}

} // namespace pipenet
