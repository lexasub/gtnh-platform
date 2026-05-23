#include "OreConfig.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <spdlog/spdlog.h>

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
        m_ores.clear();
        for (const auto& o : j["ores"]) {
            OreDef def;
            def.name = o["name"];
            def.block_id = o["block_id"];
            def.min_y = o["min_y"];
            def.max_y = o["max_y"];
            def.threshold = o["threshold"];
            def.frequency = o["frequency"];
            def.vein_size = o["vein_size"];
            def.density = o["density"];
            def.rarity = o.value("rarity", 1.0f);
            m_ores.push_back(def);
        }
        spdlog::info("[OreConfig] Loaded {} ore types from {}", m_ores.size(), path);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[OreConfig] Parse error: {}", e.what());
        return false;
    }
}

const std::vector<OreDef>& OreConfig::allOres() const { return m_ores; }

const OreDef* OreConfig::getOre(uint16_t block_id) const {
    for (const auto& o : m_ores) {
        if (o.block_id == block_id) return &o;
    }
    return nullptr;
}

int32_t OreConfig::seedOffset() const { return m_seedOffset; }