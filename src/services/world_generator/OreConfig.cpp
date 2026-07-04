#include "OreConfig.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <spdlog/spdlog.h>
#include "common/ItemId.h"

using json = nlohmann::json;

OreConfig& OreConfig::instance() {
    static OreConfig inst;
    return inst;
}

bool OreConfig::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        spdlog::error("[OreConfig] Cannot open file: {}", path);
        return false;
    }
    try {
        json j;
        file >> j;
        m_seedOffset = j.value("seed_offset", 12345);
        m_veins.clear();

        for (const auto& v : j["veins"]) {
            VeinDef def;
            def.name = v["name"];
            def.min_y = v["min_y"];
            def.max_y = v["max_y"];
            def.weight = v["weight"];
            def.density = v.value("density", 0.5f);

            def.primary_id = ItemId::pack(std::string{v["primary"]});
            def.secondary_id = ItemId::pack(std::string{v["secondary"]});
            def.sporadic_id = ItemId::pack(std::string{v["sporadic"]});

            m_veins.push_back(def);
        }
        spdlog::info("[OreConfig] Loaded {} vein types from {}", m_veins.size(), path);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[OreConfig] Parse error: {}", e.what());
        return false;
    }
}

const std::vector<VeinDef>& OreConfig::allVeins() const { return m_veins; }
int32_t OreConfig::seedOffset() const { return m_seedOffset; }