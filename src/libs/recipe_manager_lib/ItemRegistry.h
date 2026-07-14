#pragma once

#include <common/ItemId.h>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace RecipeManager {

struct ItemDefinition {
  uint16_t id;
  std::string name;
  uint8_t max_stack_size;
  uint16_t default_meta;

  ItemDefinition() : id(0), max_stack_size(64), default_meta(0) {}
  ItemDefinition(uint16_t id_, const std::string &name_, uint8_t max_stack,
                 uint16_t meta)
      : id(id_), name(name_), max_stack_size(max_stack), default_meta(meta) {}
};

class ItemRegistry {
public:
  ItemRegistry();
  ~ItemRegistry();

  bool loadFromCSV(const std::string &csvPath);
  bool loadFromSQLite(const std::string &dbPath);

  const ItemDefinition *getItem(uint16_t id) const;
  const ItemDefinition *getItemByName(const std::string &name) const;

  uint16_t nameToId(const std::string &name) const;
  std::string idToName(uint16_t id) const;
  uint8_t getMaxStackSize(uint16_t id) const;
  bool isValid(uint16_t id) const;
  size_t count() const;

  static ItemRegistry &instance();

private:
  std::unordered_map<uint16_t, ItemDefinition> itemsById_;
  std::unordered_map<std::string, uint16_t> itemsByName_;

  bool parseCSVLine(const std::string &line, std::string &idStr,
                    std::string &nameStr, std::string &stackStr,
                    std::string &metaStr) const;

  static constexpr uint16_t FALLBACK_ID = 1;
  bool loaded_ = false;
};

} // namespace RecipeManager
