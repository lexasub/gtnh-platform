#include "PatternLibrary.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace simcore {

static uint32_t xyz(uint32_t x, uint32_t y, uint32_t z) {
    return (x & 0x3FF) | ((y & 0x3FF) << 10) | ((z & 0x3FF) << 20);
}

static MultiblockPattern makeEBFPattern() {
    MultiblockPattern p;
    p.id = 1;
    p.name = "ebf";
    p.controller_block_id = 1003;
    p.size_x = 3; p.size_y = 4; p.size_z = 3;

    constexpr uint16_t CASING = 1001;
    constexpr uint16_t COIL   = 1002;
    constexpr uint16_t CTRL   = 1003;
    constexpr uint16_t ANY    = ANY_BLOCK;

    p.layers.push_back({{{CASING, CASING, CASING},
                          {CASING, CASING, CASING},
                          {CASING, CASING, CASING}}});
    p.layers.push_back({{{CASING, ANY,    CASING},
                          {ANY,    COIL,   ANY},
                          {CASING, ANY,    CASING}}});
    p.layers.push_back({{{CASING, ANY,    CASING},
                          {ANY,    COIL,   ANY},
                          {CASING, ANY,    CASING}}});
    p.layers.push_back({{{CASING, CASING, CASING},
                          {CASING, CTRL,   CASING},
                          {CASING, CASING, CASING}}});

    p.controller_dx = 1;
    p.controller_dy = 3;
    p.controller_dz = 1;

    p.hatches.push_back({1, 3, 2, HatchType::MUFFLER});
    p.hatches.push_back({1, 3, 3, HatchType::ENERGY});

    return p;
}

static MultiblockPattern makeLargeBoilerPattern() {
    MultiblockPattern p;
    p.id = 2;
    p.name = "large_boiler";
    p.controller_block_id = 1005;
    p.size_x = 3; p.size_y = 4; p.size_z = 3;

    constexpr uint16_t CASING    = 1001;
    constexpr uint16_t FIREBOX   = 1004;
    constexpr uint16_t CTRL      = 1005;
    constexpr uint16_t ANY       = ANY_BLOCK;

    p.layers.push_back({{{CASING,  CASING,  CASING},
                          {CASING,  FIREBOX, CASING},
                          {CASING,  CASING,  CASING}}});
    p.layers.push_back({{{CASING, ANY,   CASING},
                          {ANY,    ANY,   ANY},
                          {CASING, ANY,   CASING}}});
    p.layers.push_back({{{CASING, ANY,   CASING},
                          {ANY,    ANY,   ANY},
                          {CASING, ANY,   CASING}}});
    p.layers.push_back({{{CASING, CASING, CASING},
                          {CASING, CTRL,   CASING},
                          {CASING, CASING, CASING}}});

    p.controller_dx = 1;
    p.controller_dy = 3;
    p.controller_dz = 1;

    p.hatches.push_back({0, 1, 1, HatchType::FLUID_IN});
    p.hatches.push_back({2, 1, 1, HatchType::FLUID_OUT});

    return p;
}

static MultiblockPattern makeLCRPattern() {
    MultiblockPattern p;
    p.id = 3;
    p.name = "lcr";
    p.controller_block_id = 1006;
    p.size_x = 3; p.size_y = 3; p.size_z = 3;

    constexpr uint16_t CASING = 1001;
    constexpr uint16_t CTRL   = 1006;
    constexpr uint16_t ANY    = ANY_BLOCK;

    p.layers.push_back({{{CASING, CASING, CASING},
                          {CASING, CASING, CASING},
                          {CASING, CASING, CASING}}});
    p.layers.push_back({{{CASING, ANY,   CASING},
                          {ANY,    ANY,   ANY},
                          {CASING, ANY,   CASING}}});
    p.layers.push_back({{{CASING, CASING, CASING},
                          {CASING, CTRL,   CASING},
                          {CASING, CASING, CASING}}});

    p.controller_dx = 1;
    p.controller_dy = 2;
    p.controller_dz = 1;

    p.hatches.push_back({0, 1, 1, HatchType::FLUID_IN});
    p.hatches.push_back({2, 1, 1, HatchType::FLUID_OUT});
    p.hatches.push_back({1, 2, 2, HatchType::ENERGY});

    return p;
}

PatternRegistry::PatternRegistry() {
    addPattern(makeEBFPattern());
    addPattern(makeLargeBoilerPattern());
    addPattern(makeLCRPattern());
}

bool PatternRegistry::isControllerBlock(uint16_t block_id) const {
    for (const auto& [id, pattern] : patterns_) {
        if (pattern.controller_block_id == block_id) return true;
    }
    return false;
}

void PatternRegistry::addPattern(const MultiblockPattern& pattern) {
    patterns_[pattern.id] = pattern;
}

const MultiblockPattern* PatternRegistry::getPattern(uint32_t id) const {
    auto it = patterns_.find(id);
    return (it != patterns_.end()) ? &it->second : nullptr;
}

void PatternRegistry::anchorToCorner(uint32_t anchor_x, uint32_t anchor_y, uint32_t anchor_z,
                                      int32_t cdx, int32_t cdy, int32_t cdz,
                                      uint32_t& cx, uint32_t& cy, uint32_t& cz) const {
    cx = static_cast<uint32_t>(static_cast<int32_t>(anchor_x) - cdx);
    cy = static_cast<uint32_t>(static_cast<int32_t>(anchor_y) - cdy);
    cz = static_cast<uint32_t>(static_cast<int32_t>(anchor_z) - cdz);
}

bool PatternRegistry::matchLayer(const PatternLayer& layer, uint32_t corner_x, uint32_t corner_y,
                                   uint32_t corner_z, int32_t layer_y, BlockLookupFn lookup) const {
    for (size_t row = 0; row < layer.rows.size(); ++row) {
        for (size_t col = 0; col < layer.rows[row].size(); ++col) {
            uint16_t expected = layer.rows[row][col];
            if (expected == ANY_BLOCK) continue;

            uint32_t wx = corner_x + static_cast<uint32_t>(col);
            uint32_t wy = corner_y + static_cast<uint32_t>(layer_y);
            uint32_t wz = corner_z + static_cast<uint32_t>(row);

            uint16_t actual = lookup(wx, wy, wz);
            if (actual != expected) return false;
        }
    }
    return true;
}

std::vector<uint32_t> PatternRegistry::collectBlocks(const MultiblockPattern& pattern,
                                                       uint32_t corner_x, uint32_t corner_y,
                                                       uint32_t corner_z) const {
    std::vector<uint32_t> blocks;
    for (int32_t ly = 0; ly < static_cast<int32_t>(pattern.layers.size()); ++ly) {
        const auto& layer = pattern.layers[ly];
        for (size_t row = 0; row < layer.rows.size(); ++row) {
            for (size_t col = 0; col < layer.rows[row].size(); ++col) {
                blocks.push_back(xyz(
                    corner_x + static_cast<uint32_t>(col),
                    corner_y + static_cast<uint32_t>(ly),
                    corner_z + static_cast<uint32_t>(row)));
            }
        }
    }
    return blocks;
}

PatternMatchResult PatternRegistry::matchAll(uint32_t anchor_x, uint32_t anchor_y,
                                              uint32_t anchor_z, BlockLookupFn lookup) const {
    for (const auto& [id, pattern] : patterns_) {
        auto result = matchById(id, anchor_x, anchor_y, anchor_z, lookup);
        if (result.matched) return result;
    }
    return PatternMatchResult{};
}

PatternMatchResult PatternRegistry::matchById(uint32_t pattern_id, uint32_t anchor_x,
                                               uint32_t anchor_y, uint32_t anchor_z,
                                               BlockLookupFn lookup) const {
    auto* pattern = getPattern(pattern_id);
    if (!pattern) return PatternMatchResult{};

    uint32_t corner_x, corner_y, corner_z;
    anchorToCorner(anchor_x, anchor_y, anchor_z,
                   pattern->controller_dx, pattern->controller_dy, pattern->controller_dz,
                   corner_x, corner_y, corner_z);

    for (int32_t ly = 0; ly < static_cast<int32_t>(pattern->layers.size()); ++ly) {
        if (!matchLayer(pattern->layers[ly], corner_x, corner_y, corner_z, ly, lookup)) {
            return PatternMatchResult{};
        }
    }

    PatternMatchResult result;
    result.matched = true;
    result.pattern_id = pattern->id;
    result.controller_x = anchor_x;
    result.controller_y = anchor_y;
    result.controller_z = anchor_z;
    result.blocks = collectBlocks(*pattern, corner_x, corner_y, corner_z);
    return result;
}

uint16_t PatternRegistry::getBlockAtOffset(const MultiblockPattern& pattern,
                                            uint32_t anchor_x, uint32_t anchor_y, uint32_t anchor_z,
                                            int32_t dx, int32_t dy, int32_t dz,
                                            BlockLookupFn lookup) const {
    uint32_t corner_x, corner_y, corner_z;
    anchorToCorner(anchor_x, anchor_y, anchor_z,
                   pattern.controller_dx, pattern.controller_dy, pattern.controller_dz,
                   corner_x, corner_y, corner_z);
    return lookup(corner_x + static_cast<uint32_t>(dx),
                  corner_y + static_cast<uint32_t>(dy),
                  corner_z + static_cast<uint32_t>(dz));
}

} // namespace simcore
