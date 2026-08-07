#pragma once

#include <cstdint>

#include "SurfaceHeights.h"
#include "../chunk_store/Chunk/Chunk.h"
#include <common/ItemId.h>

/// Deterministic oak tree generator — stateless, pure functions of (x, z, seed).
/// Thread-safe: all mutable state is thread_local; one instance can be used
/// concurrently from multiple threads.
class TreeGenerator {
public:
    // ── tuning constants ────────────────────────────────────────────────
    static constexpr uint32_t SEED_TREES            = 0x5EED;
    static constexpr float    TREE_SCORE_THRESHOLD  = 0.12f;
    static constexpr float    SLOPE_MAX             = 1.5f;
    static constexpr float    FOREST_SCALE          = 0.004f;
    static constexpr int      MAX_TREE_H            = 7;
    static constexpr int      GRID_MARGIN           = 4;
    static constexpr int      CHUNK_SZ              = 32;
    static constexpr int      GRID                  = CHUNK_SZ + (2 * GRID_MARGIN);  // 40

    static constexpr uint16_t BLOCK_LOG    = ItemId::pack("0:10:11:2");
    static constexpr uint16_t BLOCK_LEAVES = ItemId::pack("0:10:11:3");

    explicit TreeGenerator(uint32_t worldSeed);

    /// Generate oak trees into the chunk. Must be called AFTER terrain fill
    /// and ore generation.
    void generateTrees(Chunk& c, const SurfaceHeights& surf,
                       int baseX, int baseY, int baseZ, int chunkSize);

    /// Whether a tree was placed at grid-local (gx, gz) during the last call
    /// on this thread (test support). Coordinates in [0, GRID).
    bool isTreeAt(int gx, int gz) const;

private:
    uint32_t m_seed_;
};
