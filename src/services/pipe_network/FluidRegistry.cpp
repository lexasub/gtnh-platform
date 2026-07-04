#include "FluidRegistry.h"
#include <common/ItemId.h>

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

    // IDs from data/registry/items.csv (prefix notation):
    registerFluid({ItemId::pack("1111:11:0"), "water",          1.0f,  1.0f, 373});
    registerFluid({ItemId::pack("1111:11:1"), "steam",          0.6f,  0.3f, 473});
    registerFluid({ItemId::pack("1111:11:2"), "sulfuric_acid",  1.84f, 24.0f, 610});

    // Bucket items
    registerFluid({ItemId::pack("0:1111:0"), "water",          1.0f,  1.0f, 373});
    registerFluid({ItemId::pack("0:1111:1"), "hydrogen",       0.09f, 0.01f, 20});
    registerFluid({ItemId::pack("0:1111:2"), "sulfuric_acid",  1.84f, 24.0f, 610});
}

FluidRegistry& FluidRegistry::instance() {
    static FluidRegistry inst;
    return inst;
}
