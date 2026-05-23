#pragma once
#include <cstdint>
#include <vector>
#include <utility>
#include "MachineRegistry.h"

namespace simcore {

enum class DrillState {
    IDLE,
    SEARCHING,
    MINING,
    OUTPUT_FULL
};

struct DrillComponent {
    int32_t x, y, z, tier, energyPerTick, searchLayer, searchIndex, targetX, targetY, targetZ, miningProgress, miningTicksTotal;
    std::vector<std::pair<uint16_t, uint8_t>> outputBuffer;
    DrillState state;

    // Default constructor: all zeros
    DrillComponent()
        : x(0), y(0), z(0), tier(0), energyPerTick(0), searchLayer(0), searchIndex(0),
          targetX(0), targetY(0), targetZ(0), miningProgress(0), miningTicksTotal(0),
          outputBuffer(), state(DrillState::IDLE) {}

    // Full constructor
    DrillComponent(int32_t px, int32_t py, int32_t pz, int32_t t)
        : x(px), y(py), z(pz), tier(t), energyPerTick(0), searchLayer(0), searchIndex(0),
          targetX(0), targetY(0), targetZ(0), miningProgress(0), miningTicksTotal(0),
          outputBuffer(), state(DrillState::IDLE) {
        energyPerTick = calcEnergyPerTick(t);
    }

    // Helper: start searching in a layer
    void startSearch(int32_t layer);

    // Helper: set target for mining
    void setTarget(int32_t tx, int32_t ty, int32_t tz);

    // Helper: update mining progress
    bool updateMiningProgress();

    // Helper: add output item
    bool addOutput(uint16_t itemId, uint8_t count);

    // Helper: is output buffer full?
    bool isOutputFull() const { return outputBuffer.size() >= kMaxOutputSize; }

    // Helper: is idle?
    bool isIdle() const { return state == DrillState::IDLE; }

    // Helper: is mining?
    bool isMining() const { return state == DrillState::MINING; }

    // Helper: reset component
    void reset() {
        searchIndex = 0;
        targetX = targetY = targetZ = 0;
        miningProgress = 0;
        miningTicksTotal = 0;
        outputBuffer.clear();
        state = DrillState::IDLE;
    }

    static constexpr int32_t kMaxOutputSize = 64;

    // Static helper functions
    static int32_t calcEnergyPerTick(int32_t tier);
    static int32_t calcMiningTicks(int32_t tier);
};

inline int32_t DrillComponent::calcEnergyPerTick(int32_t tier) {
    return 10 * (tier + 1) * (tier + 1);
}

inline int32_t DrillComponent::calcMiningTicks(int32_t tier) {
    return 100 / (tier + 1);
}

} // namespace simcore