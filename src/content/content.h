#pragma once
#include <cstdint>
#include <unordered_map>
#include <engine/registry/ItemId.h>

namespace content {

// --- Boiler ---
inline constexpr uint16_t kBoilerMachineId = ItemId::pack("1110:011:1");

// --- Generator ---
inline constexpr uint16_t kGeneratorMachineId_Coal = ItemId::pack("1110:000:2");
inline constexpr uint16_t kGeneratorMachineId_Steam = ItemId::pack("1110:011:0");

const std::unordered_map<uint16_t, int32_t>& generatorFuelValues();

// --- Fluids ---
inline constexpr uint16_t kFluidWater = ItemId::pack("1111:11:0");
inline constexpr uint16_t kFluidSulfuricAcid = ItemId::pack("1111:11:2");
inline constexpr uint16_t kFluidBucketWater = ItemId::pack("0:11111:0");
inline constexpr uint16_t kFluidBucketHydrogen = ItemId::pack("0:11111:1");
inline constexpr uint16_t kFluidBucketSulfuricAcid = ItemId::pack("0:11111:2");

// --- Drill / Ores ---
inline constexpr uint16_t kOreIron     = ItemId::pack("10:0");
inline constexpr uint16_t kOreGold     = ItemId::pack("10:1");
inline constexpr uint16_t kOreTin      = ItemId::pack("10:2");
inline constexpr uint16_t kOreCopper   = ItemId::pack("10:3");
inline constexpr uint16_t kOreUranium  = ItemId::pack("10:4");
inline constexpr uint16_t kOreQuartz   = ItemId::pack("10:5");
inline constexpr uint16_t kOreCoal     = ItemId::pack("10:6");
inline constexpr uint16_t kOreRedstone = ItemId::pack("10:7");
inline constexpr uint16_t kOreLapis    = ItemId::pack("10:8");
inline constexpr uint16_t kOreDiamond  = ItemId::pack("10:9");
inline constexpr uint16_t kOreElectrum = ItemId::pack("10:10");

inline constexpr uint16_t kOreBlocks[] = {
    kOreIron, kOreGold, kOreTin, kOreCopper, kOreUranium, kOreQuartz,
    kOreCoal, kOreRedstone, kOreLapis, kOreDiamond, kOreElectrum
};

const std::unordered_map<uint16_t, uint16_t>& oreDropTable();

} // namespace content
