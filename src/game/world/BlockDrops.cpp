#include "BlockDrops.h"
#include <engine/registry/ItemId.h>
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace simcore {

namespace {

// ItemId::pack() scans for digits and returns 0 when it finds none
// (ItemId.h:85-131), so a typo'd id is indistinguishable from a deliberate
// one by value alone. A zero result is therefore accepted ONLY for the two
// literal spellings of air — the same rule the sibling id-table loaders
// already apply at ItemRegistry.cpp:57-60 and ClientItemRegistry.cpp:41-43.
bool isAirIdLiteral(const std::string& s) {
  return s == "0" || s == "0:0:0";
}

// Decodes an id cell, or returns false (with a warning) if the cell is not a
// well-formed id. `which` names the column for the log message.
bool decodeId(const std::string& cell, const char* which, uint16_t& out) {
  out = ItemId::pack(cell.c_str());
  if (out != 0 || isAirIdLiteral(cell)) return true;

  spdlog::warn("BlockDrops: invalid {} id \"{}\" — skipping line", which, cell);
  return false;
}

} // namespace

BlockDrops* BlockDrops::instance_ = nullptr;

BlockDrops* BlockDrops::Load(const char* csv_path) {
  auto drops = new BlockDrops{};
  std::ifstream file(csv_path);
  if (!file.is_open()) {
    spdlog::error("BlockDrops: cannot open {}", csv_path);
    return drops;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;

    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) cols.push_back(cell);
    if (cols.size() < 2) continue;

    // Both id columns are validated. Previously only count/meta went through
    // std::stoi, so only THEY could fail — a typo like "stone,cobblestone" was
    // accepted and became a live block-0 -> item-0 (air) drop rule, which
    // emplace-first-wins then let shadow a legitimate block-0 rule.
    uint16_t source_id = 0;
    uint16_t result_id = 0;
    if (!decodeId(cols[0], "source", source_id)) continue;
    if (!decodeId(cols[1], "result", result_id)) continue;

    try {
      DropInfo info{};
      info.result_id = result_id;
      info.count = cols.size() > 2 ? static_cast<uint8_t>(std::stoi(cols[2])) : 1;
      info.meta = cols.size() > 3 ? static_cast<uint8_t>(std::stoi(cols[3])) : 0;
      drops->drops_.emplace(source_id, info);
    } catch (const std::exception&) {
      spdlog::warn("BlockDrops: skipping bad line: {}", line);
    }
  }
  spdlog::info("BlockDrops: loaded {} drop rules from {}", drops->drops_.size(),
               csv_path);
  return drops;
}

const DropInfo* BlockDrops::Get(uint16_t block_id) const {
  auto it = drops_.find(block_id);
  return it == drops_.end() ? nullptr : &it->second;
}

} // namespace simcore
