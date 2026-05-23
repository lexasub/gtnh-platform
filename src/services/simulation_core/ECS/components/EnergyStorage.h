#pragma once
#include <cstdint>
#include "MachineRegistry.h"

namespace simcore {

struct EnergyStorage {
    int32_t capacity;      // Maximum EU capacity
    int32_t current;       // Current EU stored
    int32_t maxInput;      // Maximum EU/tick input
    int32_t maxOutput;     // Maximum EU/tick output
    int32_t tier;          // GregTech tier (0=ULV, 1=LV, 2=MV, ...)
    EnergyType type = EnergyType::ELECTRICITY;

    // Default constructor: all zeros
    EnergyStorage()
        : capacity(0), current(0), maxInput(0), maxOutput(0), tier(0) {}

    // Full constructor
    EnergyStorage(int32_t cap, int32_t cur, int32_t maxIn, int32_t maxOut, int32_t t)
        : capacity(cap), current(cur), maxInput(maxIn), maxOutput(maxOut), tier(t) {}

    // Full constructor with energy type
    EnergyStorage(int32_t cap, int32_t cur, int32_t maxIn, int32_t maxOut, int32_t t, EnergyType energy_type)
        : capacity(cap), current(cur), maxInput(maxIn), maxOutput(maxOut), tier(t), type(energy_type) {}

    // Helper: add energy, returns amount actually accepted (clamped to capacity)
    int32_t addEnergy(int32_t amount);

    // Helper: consume energy, returns amount actually consumed (clamped to available)
    int32_t consumeEnergy(int32_t amount);

    // Helper: is energy full?
    bool isFull() const { return current >= capacity; }

    // Helper: is energy empty?
    bool isEmpty() const { return current <= 0; }
};

inline int32_t EnergyStorage::addEnergy(int32_t amount) {
    int32_t space = capacity - current;
    int32_t accepted = (amount < space) ? amount : space;
    if (accepted < 0) accepted = 0;
    if (accepted > maxInput) accepted = maxInput;
    current += accepted;
    return accepted;
}

inline int32_t EnergyStorage::consumeEnergy(int32_t amount) {
    int32_t available = (current < amount) ? current : amount;
    if (available > maxOutput) available = maxOutput;
    current -= available;
    return available;
}

} // namespace simcore
