#include "ItemRegistry.h"
#include <fstream>
#include <sstream>
#include <sqlite3.h>

namespace RecipeManager {

ItemRegistry::ItemRegistry() = default;

ItemRegistry::~ItemRegistry() = default;

// ---------------------------------------------------------------------------
// CSV Loading
// ---------------------------------------------------------------------------

bool ItemRegistry::loadFromCSV(const std::string& csvPath) {
    if (loaded_) return true;
    spdlog::info("Loading items from CSV: {}", csvPath);

    std::ifstream file(csvPath);
    if (!file.is_open()) {
        spdlog::error("Failed to open CSV file: {}", csvPath);
        return false;
    }

    std::string line;
    size_t lineNum = 0;
    size_t loadedCount = 0;

    while (std::getline(file, line)) {
        ++lineNum;

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Trim leading/trailing whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end = line.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start, end - start + 1);

        // Skip header row
        if (lineNum == 1 && line.find("int,name") != std::string::npos) {
            continue;
        }

        std::string idStr, nameStr, stackStr, metaStr;
        if (!parseCSVLine(line, idStr, nameStr, stackStr, metaStr)) {
            spdlog::warn("Failed to parse line {}: {}", lineNum, line);
            continue;
        }

        // Parse ID with ItemId::pack (handles "10:1" prefix notation)
        uint16_t id = ItemId::pack(idStr);
        if (id == 0 && idStr != "0" && idStr != "0:0:0") {
            spdlog::warn("Invalid item ID at line {}: {}", lineNum, idStr);
            continue;
        }

        // Stack size comes from the `stack` column of items.csv; an EMPTY cell
        // means "not specified", and the default below is the only value the
        // data supports.
        //
        // gp-70j asked for a per-subcategory default and the data does not
        // carry one. Over the 383 data rows of items.csv (comments/blanks
        // excluded) the column is: 250 empty, 81 = 1, 47 = 64, 5 = 16 (the
        // buckets), 1 = 0 (air). The populated values are coherent — 1 for
        // tools and machine blocks, 16 for buckets, 64 for ordinary items —
        // but the empty cells cut across those same subcategories: iron_pickaxe
        // and bow are 1 while wooden_pickaxe and stone_pickaxe are empty;
        // alloy_smelter_lv is 1 while heat_generator and rotare_generator are
        // empty. So the column is a partially-filled field, not a subcategory
        // rule, and item-subgroups.json (which the CSV header points at for
        // the group layout) mentions "stack" zero times. No function of name
        // prefix, id range or allocation pool reproduces the column, so a
        // per-subcategory default here would be invented, not derived.
        //
        // The 64 that remains is the repo-wide invariant rather than a guess:
        // it is the same constant in the client mirror
        // (ClientItemRegistry.cpp:53, and its GetStackSize fallback at :94),
        // and a single global 64 is what every inventory writer assumes —
        // InventoryClick.h:34, PlayerInventoryStore.cpp:84,
        // WorldContainerInventory.cpp:14, BreakBlockHandler.cpp:22. None of
        // them consult this column, so a per-subcategory default would not
        // change stacking behaviour; it would only make the registry disagree
        // with the code that actually packs items.
        //
        // TO FINISH gp-70j: populate the empty `stack` cells in items.csv, or
        // add a per-group limit to item-subgroups.json plus a lookup for it. The
        // limits are known content facts, but they are absent for 250 of 383
        // items and must not be guessed on their behalf.
        //
        // An explicit 0 in the cell is a real value, not a request for the
        // default: the cell TEXT decides, which is the house rule
        // BlockDrops.cpp:18-20 applies to an id cell, where the packed VALUE is
        // ambiguous. The client mirror stores 0 the same way.
        constexpr uint8_t kDefaultStackSize = 64;
        uint8_t stackSize = kDefaultStackSize;
        if (!stackStr.empty()) {
            try {
                stackSize = static_cast<uint8_t>(std::stoi(stackStr));
            } catch (...) {
                // keep default
            }
        }

        // Parse meta (optional, default 0)
        uint16_t meta = 0;
        if (!metaStr.empty()) {
            try {
                meta = static_cast<uint16_t>(std::stoi(metaStr));
            } catch (...) {
                // keep default
            }
        }

        ItemDefinition def(id, nameStr, idStr, stackSize, meta);

        // If same ID appears twice, log warning and use last one
        if (itemsById_.contains(id)) {
            spdlog::warn("Item ID {} already exists (line {}), replacing: {}",
                         id, lineNum, nameStr);
        }

        itemsById_[id] = def;
        itemsByName_[nameStr] = id;
        ++loadedCount;
    }

    loaded_ = loadedCount > 0;
    spdlog::info("ItemRegistry: loaded {} items from CSV", loadedCount);
    return loadedCount > 0;
}

bool ItemRegistry::parseCSVLine(const std::string& line, std::string& idStr,
                                std::string& nameStr, std::string& stackStr,
                                std::string& metaStr) const {
    std::istringstream iss(line);
    std::string field;

    // int (prefix notation or flat decimal)
    if (!std::getline(iss, field, ',')) return false;
    idStr = field;

    // name
    if (!std::getline(iss, field, ',')) return false;
    nameStr = field;

    // stack (optional)
    if (!std::getline(iss, field, ',')) return false;
    stackStr = field;

    // meta (optional)
    if (!std::getline(iss, field, ',')) return false;
    metaStr = field;

    return true;
}

// ---------------------------------------------------------------------------
// SQLite Loading
// ---------------------------------------------------------------------------

bool ItemRegistry::loadFromSQLite(const std::string& dbPath) {
    spdlog::info("Loading items from SQLite: {}", dbPath);

    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        spdlog::error("Failed to open SQLite database: {}", dbPath);
        return false;
    }

    const char* sql = "SELECT id, name, stack_size, meta FROM items";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("Failed to prepare SQL statement");
        sqlite3_close(db);
        return false;
    }

    size_t loadedCount = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        uint16_t id = static_cast<uint16_t>(sqlite3_column_int(stmt, 0));
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        uint8_t stackSize = static_cast<uint8_t>(sqlite3_column_int(stmt, 2));
        uint16_t meta = static_cast<uint16_t>(sqlite3_column_int(stmt, 3));

        ItemDefinition def(id, std::string(name), "", stackSize, meta);

        // If same ID appears twice, log warning and use last one
        if (itemsById_.count(id) > 0) {
            spdlog::warn("Item ID {} already exists in DB, replacing: {}", id, name);
        }

        itemsById_[id] = def;
        itemsByName_[def.name] = id;
        ++loadedCount;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    spdlog::info("ItemRegistry: loaded {} items from SQLite", loadedCount);
    return loadedCount > 0;
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

const ItemDefinition* ItemRegistry::getItem(uint16_t id) const {
    auto it = itemsById_.find(id);
    return (it != itemsById_.end()) ? &it->second : nullptr;
}

const ItemDefinition* ItemRegistry::getItemByName(const std::string& name) const {
    auto it = itemsByName_.find(name);
    return (it != itemsByName_.end()) ? &itemsById_.find(it->second)->second : nullptr;
}

uint16_t ItemRegistry::nameToId(const std::string& name) const {
    auto it = itemsByName_.find(name);
    if (it != itemsByName_.end()) {
        return it->second;
    }
    spdlog::warn("Item '{}' not found in registry", name);
    return 0;
}

std::string ItemRegistry::idToName(uint16_t id) const {
    auto it = itemsById_.find(id);
    return (it != itemsById_.end()) ? it->second.name : "";
}

std::string ItemRegistry::idToHierarchical(uint16_t id) const {
    auto it = itemsById_.find(id);
    return (it != itemsById_.end()) ? it->second.hierarchicalId : "";
}

uint8_t ItemRegistry::getMaxStackSize(uint16_t id) const {
    auto it = itemsById_.find(id);
    return (it != itemsById_.end()) ? it->second.max_stack_size : 64;
}

bool ItemRegistry::isValid(uint16_t id) const {
    return itemsById_.contains(id);
}

size_t ItemRegistry::count() const {
    return itemsById_.size();
}

// ---------------------------------------------------------------------------
// Singleton (Meyer's singleton)
// ---------------------------------------------------------------------------

ItemRegistry& ItemRegistry::instance() {
    static ItemRegistry instance;
    return instance;
}

} // namespace RecipeManager
