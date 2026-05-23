# Task 2: Ore Vein Configuration (ores.json)

## Objective
Create a configuration file `data/registry/ores.json` defining parameters for each ore type: height range, noise threshold, frequency, vein size.

## Requirements

### 2.1 Create ores.json
**Location**: `data/registry/ores.json`

```json
{
    "version": 1,
    "seed_offset": 12345,
    "ores": [
        {
            "name": "coal_ore",
            "block_id": 70,
            "min_y": 5,
            "max_y": 80,
            "threshold": 0.55,
            "frequency": 0.08,
            "vein_size": 3,
            "density": 0.7,
            "rarity": 1.0
        },
        {
            "name": "iron_ore",
            "block_id": 3,
            "min_y": 5,
            "max_y": 40,
            "threshold": 0.6,
            "frequency": 0.1,
            "vein_size": 3,
            "density": 0.6,
            "rarity": 1.0
        },
        {
            "name": "copper_ore",
            "block_id": 53,
            "min_y": 10,
            "max_y": 60,
            "threshold": 0.6,
            "frequency": 0.12,
            "vein_size": 2,
            "density": 0.5,
            "rarity": 1.0
        },
        {
            "name": "tin_ore",
            "block_id": 25,
            "min_y": 10,
            "max_y": 50,
            "threshold": 0.55,
            "frequency": 0.1,
            "vein_size": 2,
            "density": 0.5,
            "rarity": 1.0
        },
        {
            "name": "gold_ore",
            "block_id": 5,
            "min_y": 5,
            "max_y": 30,
            "threshold": 0.7,
            "frequency": 0.15,
            "vein_size": 2,
            "density": 0.4,
            "rarity": 0.5
        },
        {
            "name": "redstone_ore",
            "block_id": 71,
            "min_y": 5,
            "max_y": 20,
            "threshold": 0.7,
            "frequency": 0.15,
            "vein_size": 2,
            "density": 0.4,
            "rarity": 0.5
        },
        {
            "name": "lapis_ore",
            "block_id": 72,
            "min_y": 5,
            "max_y": 25,
            "threshold": 0.7,
            "frequency": 0.15,
            "vein_size": 1,
            "density": 0.3,
            "rarity": 0.3
        },
        {
            "name": "diamond_ore",
            "block_id": 73,
            "min_y": 5,
            "max_y": 15,
            "threshold": 0.8,
            "frequency": 0.2,
            "vein_size": 1,
            "density": 0.2,
            "rarity": 0.2
        }
    ]
}
```

### 2.2 Create OreConfig parser
**Location**: `src/services/world_generator/OreConfig.h/.cpp` (NEW)

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct OreDef {
    std::string name;
    uint16_t block_id;
    int32_t min_y;
    int32_t max_y;
    float threshold;      // noise threshold (lower = more ore)
    float frequency;      // noise frequency (lower = wider veins)
    uint8_t vein_size;    // max blocks in vein
    float density;        // chance of ore per noise-pass block
    float rarity;         // multiplier: 1.0 = normal, 0.5 = half as common
};

class OreConfig {
public:
    static OreConfig& instance();
    
    bool load(const std::string& path);
    const std::vector<OreDef>& allOres() const;
    const OreDef* getOre(uint16_t block_id) const;
    int32_t seedOffset() const;
    
private:
    std::vector<OreDef> m_ores;
    int32_t m_seedOffset = 12345;
};
```

### 2.3 JSON parsing (simplified)
Use existing JSON library in the project (if available) or a minimal parser. The format is simple enough for manual parsing or `nlohmann/json` if the project already has it.

## Acceptance Criteria
- [ ] `ores.json` with all 8 ore types + parameters
- [ ] `OreConfig::load()` parses JSON file correctly
- [ ] `OreConfig::getOre(block_id)` returns correct definition
- [ ] `OreConfig::allOres()` returns 8 entries
- [ ] Invalid JSON → error message, no crash
- [ ] Missing field → uses sensible default (or error)

## Dependencies
- Task 1 (ore item IDs — block_id values)
- Required by: Task 3 (generation), Task 4 (height layers)

## Files to Create/Modify
- `data/registry/ores.json` — NEW: ore configuration
- `src/services/world_generator/OreConfig.h` — NEW: parser header
- `src/services/world_generator/OreConfig.cpp` — NEW: parser implementation
