#pragma once
#include <cstdint>

namespace simcore {

    inline uint32_t xyz(uint32_t x, uint32_t y, uint32_t z) {
        return (x << 20) | (y << 10) | z;
    }

} // namespace simcore