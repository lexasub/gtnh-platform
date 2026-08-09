#include "World/BlockTransforms.h"
#include <common/ItemId.h>
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace simcore {

BlockTransforms* BlockTransforms::instance_ = nullptr;

BlockTransforms* BlockTransforms::Load(const char* csv_path) {
  auto transforms = new BlockTransforms();
  std::ifstream file(csv_path);
  if (!file.is_open()) {
    spdlog::error("BlockTransforms: cannot open {}", csv_path);
    return transforms;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;

    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) cols.push_back(cell);
    if (cols.size() < 3) continue;

    try {
      TransformResult result{};
      result.new_block_id = ItemId::pack(cols[2].c_str());
      result.new_meta =
          cols.size() > 3 ? static_cast<uint8_t>(std::stoi(cols[3])) : 0;
      uint64_t key = (static_cast<uint64_t>(ItemId::pack(cols[0].c_str())) << 16) |
                     ItemId::pack(cols[1].c_str());
      transforms->rules_.emplace(key, result);
    } catch (const std::exception&) {
      spdlog::warn("BlockTransforms: skipping bad line: {}", line);
    }
  }
  spdlog::info("BlockTransforms: loaded {} transform rules from {}",
               transforms->rules_.size(), csv_path);
  return transforms;
}

std::optional<TransformResult> BlockTransforms::Apply(
    uint16_t expected_block_id, uint16_t new_block_id) const {
  uint64_t key =
      (static_cast<uint64_t>(expected_block_id) << 16) | new_block_id;
  auto it = rules_.find(key);
  if (it == rules_.end()) return std::nullopt;
  return it->second;
}

} // namespace simcore
