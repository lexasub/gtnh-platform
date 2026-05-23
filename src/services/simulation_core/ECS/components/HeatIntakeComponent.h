#pragma once
#include <cstdint>
#include "MachineRegistry.h"

namespace simcore {
struct HeatIntakeComponent {
    EnergyType input_type = EnergyType::HEAT;
    int32_t heat_stored = 0;
    int32_t heat_capacity = 1000;
    float ratio() const {
        return (heat_capacity > 0) ? static_cast<float>(heat_stored) / static_cast<float>(heat_capacity) : 0.0f;
    }
};
} // namespace simcore