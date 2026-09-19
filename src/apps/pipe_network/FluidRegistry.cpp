#include "FluidRegistry.h"
#include "content/content.h"
#include <engine/registry/Registry.h>

FluidRegistry::FluidRegistry() {
    initDefaults();
}

void FluidRegistry::registerFluid(const FluidDef& def) {
    m_fluids[def.item_id] = def;
}

const FluidDef* FluidRegistry::getFluid(uint16_t item_id) const {
    auto it = m_fluids.find(item_id);
    return it != m_fluids.end() ? &it->second : nullptr;
}

bool FluidRegistry::isFluid(uint16_t item_id) const {
    return m_fluids.count(item_id) > 0;
}

void FluidRegistry::initDefaults() {
    if (initialized_) return;
    initialized_ = true;

    // IDs from data/registry/items.csv (prefix notation). The Steam id comes
    // from the shared canonical registry when one loads; otherwise the pinned
    // constant that registry_test keeps equal to the resolved value is used,
    // so steam properties are always registered under the canonical id.
    gtnh::common::Registry shared;
    uint16_t steam_id = gtnh::common::steamItemId();
    if (shared.load("src/content/data/registry")) {
        const uint16_t resolved = shared.steamItemId();
        if (resolved != 0) steam_id = resolved;
    }
    registerFluid({content::kFluidWater, "water",          1.0f,  1.0f, 373});
    registerFluid({steam_id,                  "steam",          0.6f,  0.3f, 473});
    registerFluid({content::kFluidSulfuricAcid, "sulfuric_acid",  1.84f, 24.0f, 610});

    // Bucket items
    registerFluid({content::kFluidBucketWater, "water",          1.0f,  1.0f, 373});
    registerFluid({content::kFluidBucketHydrogen, "hydrogen",       0.09f, 0.01f, 20});
    registerFluid({content::kFluidBucketSulfuricAcid, "sulfuric_acid",  1.84f, 24.0f, 610});
}

FluidRegistry& FluidRegistry::instance() {
    static FluidRegistry inst;
    return inst;
}
