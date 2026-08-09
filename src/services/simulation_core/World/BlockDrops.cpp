#include "World/BlockDrops.h"
#include <common/ItemId.h>
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace simcore {

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

    try {
      DropInfo info{};
      info.result_id = ItemId::pack(cols[1].c_str());
      info.count = cols.size() > 2 ? static_cast<uint8_t>(std::stoi(cols[2])) : 1;
      info.meta = cols.size() > 3 ? static_cast<uint8_t>(std::stoi(cols[3])) : 0;
      drops->drops_.emplace(ItemId::pack(cols[0].c_str()), info);
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
