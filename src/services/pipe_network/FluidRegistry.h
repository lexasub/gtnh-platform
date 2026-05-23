#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

struct FluidDef {
    uint16_t item_id;
    std::string name;
    float density;
    float viscosity;
    uint16_t max_temp;
};

class FluidRegistry {
public:
    FluidRegistry();

    void registerFluid(const FluidDef& def);
    const FluidDef* getFluid(uint16_t item_id) const;
    bool isFluid(uint16_t item_id) const;
    static FluidRegistry& instance();
    void initDefaults();
private:
    bool initialized_ = false;
    std::unordered_map<uint16_t, FluidDef> m_fluids;
};
