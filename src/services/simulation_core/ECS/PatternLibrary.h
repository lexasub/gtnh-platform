#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace simcore {

constexpr uint16_t ANY_BLOCK = 0xFFFF;

enum class HatchType : uint8_t {
    NONE = 0,
    ITEM_IN = 1,
    ITEM_OUT = 2,
    FLUID_IN = 3,
    FLUID_OUT = 4,
    ENERGY = 5,
    MUFFLER = 6,
};

struct PatternLayer {
    std::vector<std::vector<uint16_t>> rows;
};

struct HatchDef {
    int32_t dx, dy, dz;
    HatchType type;
};

struct PatternMatchResult {
    bool matched = false;
    uint32_t pattern_id = 0;
    uint32_t controller_x = 0;
    uint32_t controller_y = 0;
    uint32_t controller_z = 0;
    std::vector<uint32_t> blocks;
};

struct MultiblockPattern {
    uint32_t id = 0;
    std::string name;
    uint16_t controller_block_id = 0;
    uint8_t size_x = 0, size_y = 0, size_z = 0;
    std::vector<PatternLayer> layers;
    std::vector<HatchDef> hatches;
    int32_t controller_dx = 0;
    int32_t controller_dy = 0;
    int32_t controller_dz = 0;
};

using BlockLookupFn = std::function<uint16_t(uint32_t x, uint32_t y, uint32_t z)>;

constexpr uint16_t HATCH_BLOCK_ITEM_IN  = 1003;
constexpr uint16_t HATCH_BLOCK_ITEM_OUT = 1004;
constexpr uint16_t HATCH_BLOCK_FLUID_IN = 1005;
constexpr uint16_t HATCH_BLOCK_FLUID_OUT = 1006;

inline HatchType hatchBlockIdToType(uint16_t block_id) {
    switch (block_id) {
        case HATCH_BLOCK_ITEM_IN:  return HatchType::ITEM_IN;
        case HATCH_BLOCK_ITEM_OUT: return HatchType::ITEM_OUT;
        case HATCH_BLOCK_FLUID_IN: return HatchType::FLUID_IN;
        case HATCH_BLOCK_FLUID_OUT: return HatchType::FLUID_OUT;
        default: return HatchType::NONE;
    }
}

class PatternRegistry {
public:
    PatternRegistry();

    void addPattern(const MultiblockPattern& pattern);
    const MultiblockPattern* getPattern(uint32_t id) const;

    PatternMatchResult matchAll(uint32_t anchor_x, uint32_t anchor_y, uint32_t anchor_z,
                                 BlockLookupFn lookup) const;

    PatternMatchResult matchById(uint32_t pattern_id,
                                   uint32_t anchor_x, uint32_t anchor_y, uint32_t anchor_z,
                                   BlockLookupFn lookup) const;

    struct HatchResult {
      int32_t world_x, world_y, world_z;
      HatchType type;
    };

    std::vector<HatchResult> findHatches(
        uint32_t controller_world_x, uint32_t controller_world_y, uint32_t controller_world_z,
        const MultiblockPattern& pattern, BlockLookupFn lookup) const;

    uint16_t getBlockAtOffset(const MultiblockPattern& pattern,
                               uint32_t anchor_x, uint32_t anchor_y, uint32_t anchor_z,
                               int32_t dx, int32_t dy, int32_t dz,
                               BlockLookupFn lookup) const;

    bool isControllerBlock(uint16_t block_id) const;
    size_t size() const { return patterns_.size(); }

private:
    std::unordered_map<uint32_t, MultiblockPattern> patterns_;

    void anchorToCorner(uint32_t anchor_x, uint32_t anchor_y, uint32_t anchor_z,
                         int32_t cdx, int32_t cdy, int32_t cdz,
                         uint32_t& cx, uint32_t& cy, uint32_t& cz) const;

    bool matchLayer(const PatternLayer& layer, uint32_t corner_x, uint32_t corner_y, uint32_t corner_z,
                    int32_t layer_y, BlockLookupFn lookup) const;

    std::vector<uint32_t> collectBlocks(const MultiblockPattern& pattern,
                                         uint32_t corner_x, uint32_t corner_y, uint32_t corner_z) const;
};

} // namespace simcore
