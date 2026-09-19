#pragma once

#include <cstdint>

/// Single owner of the terrain height formula.
/// Thread-safe: fill() uses thread_local noise nodes; multiple threads
/// may call fill() concurrently with different buffers and coordinates.
class SurfaceHeights {
public:
    explicit SurfaceHeights(uint32_t worldSeed);

    /// Fill `out` (size×size floats, row-major: index = z*size + x)
    /// with terrain heights for world columns
    /// [baseX, baseX+size) × [baseZ, baseZ+size).
    void fill(float* out, int size, int baseX, int baseZ) const;

    /// Single-column height query (convenience).
    [[nodiscard]] float at(int wx, int wz) const;

private:
    uint32_t seed_;
};
