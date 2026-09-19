// GTNH content pack (L0): the single C++ place naming concrete ids and
// balance tables (openspec: refactor-engine-layer-separation, task 3.5).
// Machinery in game/ receives them via content.h.
#include "content.h"

namespace content {

const std::unordered_map<uint16_t, int32_t>& generatorFuelValues() {
    static const std::unordered_map<uint16_t, int32_t> kFuel = {
        {ItemId::pack("0:11110:2"), 8000},   // coal
        {ItemId::pack("0:10:00:0"), 2000},   // oak_planks
        {ItemId::pack("0:11110:0"), 500},    // stick
    };
    return kFuel;
}

const std::unordered_map<uint16_t, uint16_t>& oreDropTable() {
    static const std::unordered_map<uint16_t, uint16_t> kDrops = {
        {kOreIron,     ItemId::pack("0:110:1")},        // iron ingot
        {kOreGold,     ItemId::pack("0:110:2")},        // gold ingot
        {kOreTin,      ItemId::pack("0:110:3")},        // tin ingot
        {kOreElectrum, ItemId::pack("0:110:4")},        // electrum ingot
        {kOreCopper,   ItemId::pack("0:1110:001:24")},  // copper ingot
        {kOreUranium,  ItemId::pack("0:110:5")},        // uranium ingot
        {kOreQuartz,   ItemId::pack("0:1110:101:0")},   // quartz
        {kOreCoal,     ItemId::pack("0:11110:2")},      // coal
        // redstone / lapis / diamond: no drop (old switch returned 0;
        // absence from the map resolves to the same default).
    };
    return kDrops;
}

} // namespace content
