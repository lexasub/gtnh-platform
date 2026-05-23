#include "FluidRegistry.h"

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

    // IDs must match data/registry/items.csv:
    //   water=84, steam=85, sulfuric_acid=86
    registerFluid({84, "water",          1.0f,  1.0f, 373});
    registerFluid({85, "steam",          0.6f,  0.3f, 473});
    registerFluid({86, "sulfuric_acid",  1.84f, 24.0f, 610});

    // Additional fluids from items.csv (bucket items)
    registerFluid({16, "water",          1.0f,  1.0f, 373});     // water_bucket
    registerFluid({17, "hydrogen",       0.09f, 0.01f, 20});     // hydrogen_bucket
    registerFluid({18, "sulfuric_acid",  1.84f, 24.0f, 610});    // sulfuric_acid_bucket
}

FluidRegistry& FluidRegistry::instance() {
    static FluidRegistry inst;
    return inst;
}
