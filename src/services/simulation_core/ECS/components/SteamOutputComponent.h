#pragma once

#include "MachineRegistry.h"
#include <cstdint>

namespace simcore {

// Stores steam produced by boilers (heat boiler converts neighbour HEAT->STEAM,
// solid boiler via GeneratorSystem burning fuel). Capacity-based, like HeatIntakeComponent.
struct SteamOutputComponent {
    EnergyType input_type = EnergyType::STEAM;
    double steam_stored = 0.0;
    double steam_capacity = 1000.0;

    inline double ratio() const {
        return steam_capacity > 0 ? steam_stored / steam_capacity : 0.0;
    }
};

} // namespace simcore
