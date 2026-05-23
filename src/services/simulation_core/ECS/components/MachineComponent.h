#pragma once
#include <cstdint>
#include "SideConfig.h"

namespace simcore {

enum class MachineFaceRole : uint8_t {
    NONE    = 0,
    INPUT   = 1,
    OUTPUT  = 2,
    ENERGY  = 3,
    FLUID_IN = 4,
    FLUID_OUT = 5
};

struct MachineComponent {
    uint16_t machine_id = 0;    // machine type ID from MachineRegistry
    uint32_t mb_id = 0;         // 0 for single-block, nonzero for multiblock machines
    uint32_t x = 0;             // block position (world coordinates)
    uint32_t y = 0;
    uint32_t z = 0;
    uint64_t machine_instance_id = 0;    // unique machine ID
    bool managed_externally = false;     // true = recipe handled by reciped, not MachineSystem
    uint8_t side_config[6] = {0, 0, 0, 0, 0, 0};

    // Default constructor: all zeros
    MachineComponent() = default;
    // Full constructor — accepts machine_id directly from MachineRegistry
    MachineComponent(uint16_t type, uint32_t mb, uint32_t px, uint32_t py, uint32_t pz, uint64_t mid)
        : machine_id(type), mb_id(mb), x(px), y(py), z(pz), machine_instance_id(mid) {};
    
    // Face roles: DOWN(0), UP(1), NORTH(2), SOUTH(3), WEST(4), EAST(5)
    void setFaceRole(uint8_t face, uint8_t role) {
        if (face < 6) side_config[face] = role;
    }
    
    uint8_t getFaceRole(uint8_t face) const {
        return (face < 6) ? side_config[face] : 0;
    }
};

} // namespace simcore
