#include "Registry.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace gtnh::common {
namespace {

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::vector<std::string> fields(const std::string& line) {
  std::vector<std::string> result;
  std::stringstream stream(line);
  std::string field;
  while (std::getline(stream, field, ',')) result.push_back(trim(field));
  if (!line.empty() && line.back() == ',') result.emplace_back();
  return result;
}

bool integer(std::string_view text, int& value) {
  if (text.empty()) return false;
  const char* first = text.data();
  const char* last = first + text.size();
  auto result = std::from_chars(first, last, value);
  return result.ec == std::errc{} && result.ptr == last;
}

bool real(std::string_view text, double& value) {
  try {
    std::size_t used = 0;
    value = std::stod(std::string(text), &used);
    return used == text.size();
  } catch (...) {
    return false;
  }
}

bool boolean(std::string_view text, bool& value) {
  if (text == "true") { value = true; return true; }
  if (text == "false") { value = false; return true; }
  return false;
}

bool header(const std::vector<std::string>& row, std::initializer_list<std::string_view> expected) {
  if (row.size() != expected.size()) return false;
  auto it = expected.begin();
  for (const auto& value : row) {
    if (value != *it++) return false;
  }
  return true;
}

} // namespace

void Registry::clear() {
  items_.clear(); names_.clear(); pipes_.clear(); cables_.clear(); fluids_.clear();
  drops_.clear(); errors_.clear();
}

void Registry::error(std::size_t line, std::string_view file, std::string_view message) {
  errors_.emplace_back(std::string(file) + ":" + std::to_string(line) + ": " + std::string(message));
}

bool Registry::parseItemId(std::string_view text, std::uint16_t& id) {
  if (text.empty()) return false;
  // ItemId::pack historically accepts malformed strings. The shared loader is
  // deliberately stricter so malformed references cannot silently become zero.
  for (char c : text) {
    if (c != ':' && (c < '0' || c > '9')) return false;
  }
  const auto separator = text.rfind(':');
  if (separator == 0 || separator == text.size() - 1) return false;
  if (separator == std::string_view::npos) {
    int value = 0;
    if (!integer(text, value) || value < 0 || value > 65535) return false;
    id = static_cast<std::uint16_t>(value);
    return true;
  }
  for (std::size_t i = 0; i < separator; ++i) {
    if (text[i] != ':' && text[i] != '0' && text[i] != '1') return false;
  }
  int payload = 0;
  if (!integer(text.substr(separator + 1), payload) || payload < 0 || payload > 65535) return false;
  id = ItemId::pack(text);
  return id != 0 || (text == "0" || text == "0:0:0");
}

bool Registry::load(std::string_view directory) {
  const std::filesystem::path root(directory);
  return load((root / "items.csv").string(), (root / "pipes.csv").string(),
              (root / "cables.csv").string(), (root / "fluids.csv").string(),
              (root / "drops.csv").string());
}

bool Registry::load(std::string_view items, std::string_view pipes,
                    std::string_view cables, std::string_view fluids,
                    std::string_view drops) {
  clear();
  const bool loaded = loadItems(items) && loadPipes(pipes) && loadCables(cables) &&
                      loadFluids(fluids) && loadDrops(drops);
  if (loaded) validateReferences();
  if (!valid()) {
    // Do not expose a partially valid catalog to callers after a failed startup.
    items_.clear(); names_.clear(); pipes_.clear(); cables_.clear(); fluids_.clear(); drops_.clear();
  }
  return valid();
}

bool Registry::loadItems(std::string_view path) {
  std::ifstream input{std::string(path)};
  if (!input) { error(0, path, "cannot open file"); return false; }
  std::string line; std::size_t line_no = 0; bool first = true;
  while (std::getline(input, line)) {
    ++line_no; line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    const auto row = fields(line);
    if (first) { first = false; if (header(row, {"int", "name", "stack", "meta"})) continue; }
    if (row.size() != 4) { error(line_no, path, "items row must have 4 columns"); continue; }
    std::uint16_t id = 0;
    if (!parseItemId(row[0], id)) { error(line_no, path, "invalid item id"); continue; }
    int stack = 64, meta = 0;
    if (!row[2].empty() && (!integer(row[2], stack) || stack < 0 || stack > 65535)) error(line_no, path, "invalid stack size");
    if (!row[3].empty() && (!integer(row[3], meta) || meta < 0 || meta > 65535)) error(line_no, path, "invalid meta");
    if (row[1].empty()) { error(line_no, path, "empty item name"); continue; }
    if (items_.find(id) != items_.end()) { error(line_no, path, "duplicate item id"); continue; }
    if (names_.find(row[1]) != names_.end()) { error(line_no, path, "duplicate item name"); continue; }
    items_.emplace(id, ItemDefinition{id, row[0], row[1], static_cast<std::uint16_t>(stack), static_cast<std::uint16_t>(meta)});
    names_.emplace(row[1], id);
  }
  return true;
}

bool Registry::loadPipes(std::string_view path) {
  std::ifstream input{std::string(path)};
  if (!input) { error(0, path, "cannot open file"); return false; }
  std::string line; std::size_t line_no = 0; bool first = true;
  while (std::getline(input, line)) {
    ++line_no; line = trim(line); if (line.empty() || line[0] == '#') continue;
    const auto row = fields(line);
    if (first) { first = false; if (header(row, {"item_id", "name", "tier", "flow_rate", "items_per_sec"})) continue; }
    if (row.size() != 5) { error(line_no, path, "pipes row must have 5 columns"); continue; }
    std::uint16_t id = 0; int tier = 0, flow = 0, item_rate = 0;
    if (!parseItemId(row[0], id) || !integer(row[2], tier) || !integer(row[3], flow) || !integer(row[4], item_rate)) { error(line_no, path, "invalid pipe row"); continue; }
    if (pipes_.find(id) != pipes_.end()) error(line_no, path, "duplicate pipe item id");
    pipes_.emplace(id, PipeDefinition{id, row[1], tier, flow, item_rate});
  }
  return true;
}

bool Registry::loadCables(std::string_view path) {
  std::ifstream input{std::string(path)};
  if (!input) { error(0, path, "cannot open file"); return false; }
  std::string line; std::size_t line_no = 0; bool first = true;
  while (std::getline(input, line)) {
    ++line_no; line = trim(line); if (line.empty() || line[0] == '#') continue;
    const auto row = fields(line);
    if (first) { first = false; if (header(row, {"item_id", "name", "tier", "loss", "ampacity", "insulation"})) continue; }
    if (row.size() != 6) { error(line_no, path, "cables row must have 6 columns"); continue; }
    std::uint16_t id = 0; int tier = 0, ampacity = 0, insulation = 0; double loss = 0;
    if (!parseItemId(row[0], id) || !integer(row[2], tier) || !real(row[3], loss) || !integer(row[4], ampacity) || !integer(row[5], insulation)) { error(line_no, path, "invalid cable row"); continue; }
    if (cables_.find(id) != cables_.end()) error(line_no, path, "duplicate cable item id");
    cables_.emplace(id, CableDefinition{id, row[1], tier, loss, ampacity, insulation});
  }
  return true;
}

bool Registry::loadFluids(std::string_view path) {
  std::ifstream input{std::string(path)};
  if (!input) { error(0, path, "cannot open file"); return false; }
  std::string line; std::size_t line_no = 0; bool first = true;
  while (std::getline(input, line)) {
    ++line_no; line = trim(line); if (line.empty() || line[0] == '#') continue;
    const auto row = fields(line);
    if (first) { first = false; if (header(row, {"fluid_id", "item_id", "name", "color", "density", "gaseous", "temperature"})) continue; }
    if (row.size() != 7) { error(line_no, path, "fluids row must have 7 columns"); continue; }
    int fluid_id = 0, temperature = 0; std::uint16_t item_id = 0; double density = 0; bool gaseous = false;
    if (!integer(row[0], fluid_id) || fluid_id <= 0 || fluid_id > 65535 || !parseItemId(row[1], item_id) || !real(row[4], density) || !boolean(row[5], gaseous) || !integer(row[6], temperature)) { error(line_no, path, "invalid fluid row"); continue; }
    if (fluids_.find(static_cast<std::uint16_t>(fluid_id)) != fluids_.end()) { error(line_no, path, "duplicate fluid id"); continue; }
    fluids_.emplace(static_cast<std::uint16_t>(fluid_id), FluidDefinition{static_cast<std::uint16_t>(fluid_id), item_id, row[2], row[3], density, gaseous, temperature});
  }
  return true;
}

bool Registry::loadDrops(std::string_view path) {
  std::ifstream input{std::string(path)};
  if (!input) { error(0, path, "cannot open file"); return false; }
  std::string line; std::size_t line_no = 0;
  while (std::getline(input, line)) {
    ++line_no; line = trim(line); if (line.empty() || line[0] == '#') continue;
    const auto row = fields(line); if (row.size() < 2 || row.size() > 4) { error(line_no, path, "drops row must have 2 to 4 columns"); continue; }
    std::uint16_t source = 0, result = 0; int count = 1, meta = 0;
    if (!parseItemId(row[0], source) || !parseItemId(row[1], result) || (row.size() > 2 && !integer(row[2], count)) || (row.size() > 3 && !integer(row[3], meta))) { error(line_no, path, "invalid drop row"); continue; }
    drops_.push_back(DropDefinition{source, result, count, meta});
  }
  return true;
}

bool Registry::validateReferences() {
  for (const auto& [id, row] : pipes_) {
    if (items_.find(id) == items_.end()) error(0, "pipes.csv", "item_id does not resolve in items.csv");
    else if (items_.at(id).name != row.name) error(0, "pipes.csv", "name does not match items.csv");
  }
  for (const auto& [id, row] : cables_) {
    if (items_.find(id) == items_.end()) error(0, "cables.csv", "item_id does not resolve in items.csv");
    else if (items_.at(id).name != row.name) error(0, "cables.csv", "name does not match items.csv");
  }
  for (const auto& [id, row] : fluids_) {
    if (items_.find(row.item_id) == items_.end()) error(0, "fluids.csv", "item_id does not resolve in items.csv");
    else if (items_.at(row.item_id).name != row.name) error(0, "fluids.csv", "name does not match items.csv");
  }
  for (const auto& row : drops_) {
    if (items_.find(row.source) == items_.end()) error(0, "drops.csv", "source does not resolve in items.csv");
    if (items_.find(row.result) == items_.end()) error(0, "drops.csv", "result does not resolve in items.csv");
  }
  return errors_.empty();
}

const ItemDefinition* Registry::item(std::uint16_t id) const { auto it = items_.find(id); return it == items_.end() ? nullptr : &it->second; }
const ItemDefinition* Registry::itemByName(std::string_view name) const { auto it = names_.find(std::string(name)); return it == names_.end() ? nullptr : item(it->second); }
const PipeDefinition* Registry::pipe(std::uint16_t id) const { auto it = pipes_.find(id); return it == pipes_.end() ? nullptr : &it->second; }
const CableDefinition* Registry::cable(std::uint16_t id) const { auto it = cables_.find(id); return it == cables_.end() ? nullptr : &it->second; }
const FluidDefinition* Registry::fluid(std::uint16_t id) const { auto it = fluids_.find(id); return it == fluids_.end() ? nullptr : &it->second; }
const FluidDefinition* Registry::fluidByItem(std::uint16_t id) const { for (const auto& [_, row] : fluids_) if (row.item_id == id) return &row; return nullptr; }
std::uint16_t Registry::steamItemId() const { const auto* steam = itemByName("steam"); return steam == nullptr ? 0 : steam->id; }

} // namespace gtnh::common
