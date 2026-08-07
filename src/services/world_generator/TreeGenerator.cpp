#include "TreeGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "FastNoise/FastNoise.h"

namespace {
    // ── noise nodes (thread-local, configured once) ────────────────────
    thread_local FastNoise::SmartNode<FastNoise::Simplex>    fnForestSimplex;
    thread_local FastNoise::SmartNode<FastNoise::FractalFBm> forestFBM;
    thread_local bool forestInit = false;

    void initForest() {
        if (forestInit) [[likely]] return;
        fnForestSimplex = FastNoise::New<FastNoise::Simplex>();
        fnForestSimplex->SetScale(1.0f / TreeGenerator::FOREST_SCALE);
        forestFBM = FastNoise::New<FastNoise::FractalFBm>();
        forestFBM->SetSource(fnForestSimplex);
        forestFBM->SetOctaveCount(3);
        forestFBM->SetLacunarity(2.0f);
        forestFBM->SetGain(0.5f);
        forestInit = true;
    }

    // ── thread-local precomputed grids (GRID × GRID) ───────────────────
    constexpr int GRID = TreeGenerator::GRID;
    constexpr int GRID_SQ = GRID * GRID;
    constexpr int MARGIN = TreeGenerator::GRID_MARGIN;

    thread_local std::array<float,    GRID_SQ> hmap_;
    thread_local std::array<float,    GRID_SQ> scoreMap_;
    thread_local std::array<bool,     GRID_SQ> treeMap_;
    thread_local std::array<float,    GRID_SQ> forestBuf_;   // forestFBM 2D output
    thread_local std::array<float,    GRID_SQ> densityBuf_;  // density per cell

    inline int idx(int gx, int gz) { return gz * GRID + gx; }

    // ── deterministic hash (same shape as OreGenerator::hashRegion) ─────
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)"
    uint32_t hashTree(int wx, int wz, uint32_t seed) {
        uint32_t h = seed ^ (static_cast<uint32_t>(wx) * 374761393U)
                          ^ (static_cast<uint32_t>(wz) * 668265263U);
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }

    // ── maximum / minimum over grid ─────────────────────────────────────
    float gridMax() {
        float m = hmap_[0];
        for (int i = 1; i < GRID_SQ; ++i)
            if (hmap_[i] > m) m = hmap_[i];
        return m;
    }
    float gridMin() {
        float m = hmap_[0];
        for (int i = 1; i < GRID_SQ; ++i)
            if (hmap_[i] < m) m = hmap_[i];
        return m;
    }

    // ── slope check ────────────────────────────────────────────────────
    bool slopeOK(int gi) {
        int gx = gi % GRID;
        int gz = gi / GRID;
        float h = hmap_[gi];
        // 4 axis neighbors, clamped to grid bounds
        if (gx > 0        && std::abs(hmap_[idx(gx-1, gz)] - h) > TreeGenerator::SLOPE_MAX) return false;
        if (gx < GRID - 1 && std::abs(hmap_[idx(gx+1, gz)] - h) > TreeGenerator::SLOPE_MAX) return false;
        if (gz > 0        && std::abs(hmap_[idx(gx, gz-1)] - h) > TreeGenerator::SLOPE_MAX) return false;
        if (gz < GRID - 1 && std::abs(hmap_[idx(gx, gz+1)] - h) > TreeGenerator::SLOPE_MAX) return false;
        return true;
    }

    // ── strict 5×5 local maximum ────────────────────────────────────────
    bool isStrictLocalMax(int gi, float score) {
        int gx = gi % GRID;
        int gz = gi / GRID;
        for (int dz = -2; dz <= 2; ++dz) {
            for (int dx = -2; dx <= 2; ++dx) {
                if (dx == 0 && dz == 0) continue;
                int nx = gx + dx;
                int nz = gz + dz;
                if (nx < 0 || nx >= GRID || nz < 0 || nz >= GRID) continue;
                if (scoreMap_[idx(nx, nz)] >= score) return false;   // strict: >= blocks
            }
        }
        return true;
    }
} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
TreeGenerator::TreeGenerator(uint32_t worldSeed) : m_seed_(worldSeed) {}

bool TreeGenerator::isTreeAt(int gx, int gz) const {
    if (gx < 0 || gx >= GRID || gz < 0 || gz >= GRID) return false;
    return treeMap_[idx(gx, gz)];
}

void TreeGenerator::generateTrees(Chunk& c, const SurfaceHeights& surf,
                                  int baseX, int baseY, int baseZ, int chunkSize) {
    initForest();

    const uint32_t forestSeed = m_seed_ + SEED_TREES;
    const int gridBaseX = baseX - MARGIN;
    const int gridBaseZ = baseZ - MARGIN;

    // ── 1. Precompute hmap_ from SurfaceHeights ──────────────────────────
    surf.fill(hmap_.data(), GRID, gridBaseX, gridBaseZ);

    // ── 2. Early exit: chunk entirely above or below the surface ─────────
    if (gridMax() < static_cast<float>(baseY) ||
        gridMin() > static_cast<float>(baseY + chunkSize)) {
        treeMap_.fill(false);
        return;
    }

    // ── 3. forestFBM → density → score → treeMap_ ───────────────────────
    forestFBM->GenUniformGrid2D(forestBuf_.data(), gridBaseX, gridBaseZ,
                                 GRID, GRID, 1.0f, 1.0f, forestSeed);

    for (int i = 0; i < GRID_SQ; ++i) {
        densityBuf_[i] = 0.5f + 0.5f * forestBuf_[i];          // [0, 1]
    }

    for (int i = 0; i < GRID_SQ; ++i) {
        int gx = i % GRID;
        int gz = i / GRID;
        int wx = gridBaseX + gx;
        int wz = gridBaseZ + gz;
        float chance = static_cast<float>(hashTree(wx, wz, forestSeed) & 0xFFFF) / 65535.0f;
        scoreMap_[i] = chance * densityBuf_[i];
    }

    for (int i = 0; i < GRID_SQ; ++i) {
        treeMap_[i] = (scoreMap_[i] > TREE_SCORE_THRESHOLD) &&
                      isStrictLocalMax(i, scoreMap_[i]) &&
                      slopeOK(i);
    }

    // ── 4. Trunk pass ────────────────────────────────────────────────────
    // Only process cells that belong to the actual chunk (offset by MARGIN).
    for (int gz = MARGIN; gz < MARGIN + chunkSize; ++gz) {
        for (int gx = MARGIN; gx < MARGIN + chunkSize; ++gx) {
            int gi = idx(gx, gz);
            if (!treeMap_[gi]) continue;

            int wx = gridBaseX + gx;
            int wz = gridBaseZ + gz;
            float surface = hmap_[gi];

            int trunkH = 4 + static_cast<int>((hashTree(wx, wz, forestSeed) >> 8) % 4); // 4..7
            int trunkTop = static_cast<int>(surface) + 1 + trunkH;

            for (int wy = static_cast<int>(surface) + 1; wy < trunkTop; ++wy) {
                if (wy < baseY || wy >= baseY + chunkSize) continue;
                int ly = wy - baseY;
                int lz = gz - MARGIN;
                int lx = gx - MARGIN;
                c.blocks[(ly * chunkSize + lz) * chunkSize + lx] = BLOCK_LOG;
            }
        }
    }

    // ── 5. Canopy pass ───────────────────────────────────────────────────
    // For each column of the chunk, check all treeMap_ cells within
    // Chebyshev radius 2 and place leaves where the ellipsoid holds.
    for (int lz = 0; lz < chunkSize; ++lz) {
        for (int lx = 0; lx < chunkSize; ++lx) {
            int gx = MARGIN + lx;
            int gz = MARGIN + lz;

            // Scan the 5×5 neighborhood of tree candidate cells.
            for (int tdz = -2; tdz <= 2; ++tdz) {
                for (int tdx = -2; tdx <= 2; ++tdx) {
                    int tgx = gx + tdx;
                    int tgz = gz + tdz;
                    if (tgx < 0 || tgx >= GRID || tgz < 0 || tgz >= GRID) continue;
                    if (!treeMap_[idx(tgx, tgz)]) continue;
                    // Trunk column of this tree is at (tgx, tgz) — skip own trunk.
                    if (tdx == 0 && tdz == 0) continue;

                    int twx = gridBaseX + tgx;
                    int twz = gridBaseZ + tgz;
                    float tSurface = hmap_[idx(tgx, tgz)];
                    int tH = 4 + static_cast<int>((hashTree(twx, twz, forestSeed) >> 8) % 4);
                    int tTop = static_cast<int>(tSurface) + 1 + tH;
                    float centerY = static_cast<float>(tTop - 1);

                    // Ellipsoid: (dx²+dz²)/4 + (wy−centerY)²/2.25 ≤ 1
                    float dx = static_cast<float>(tdx);
                    float dz = static_cast<float>(tdz);
                    float r2xy = (dx * dx + dz * dz) / 4.0f;

                    // Pre-check: if r2xy > 1 this tree contributes nothing here.
                    if (r2xy > 1.0f) continue;

                    int minWy = std::max(baseY,
                                         static_cast<int>(std::ceil(centerY - 1.5f)));
                    int maxWy = std::min(baseY + chunkSize - 1,
                                         static_cast<int>(std::floor(centerY + 1.5f)));

                    for (int wy = minWy; wy <= maxWy; ++wy) {
                        float dy = static_cast<float>(wy) - centerY;
                        if (r2xy + (dy * dy) / 2.25f > 1.0f) continue;

                        int ly = wy - baseY;
                        uint16_t& blk = c.blocks[(ly * chunkSize + lz) * chunkSize + lx];
                        if (blk == 0) {   // AIR only
                            blk = BLOCK_LEAVES;
                        }
                    }
                }
            }
        }
    }
}
