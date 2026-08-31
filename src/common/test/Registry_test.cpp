#include <common/Registry.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <utility>
#include <string>

using gtnh::common::Registry;
using namespace ItemId;

namespace {
std::filesystem::path tempRegistry(const std::string& suffix, const std::string& items,
                                   const std::string& pipes, const std::string& cables,
                                   const std::string& fluids, const std::string& drops) {
  auto root = std::filesystem::temp_directory_path() / ("gtnh-registry-" + suffix);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const std::pair<const char*, const std::string> files[] = {
      {"items.csv", items}, {"pipes.csv", pipes}, {"cables.csv", cables},
      {"fluids.csv", fluids}, {"drops.csv", drops}};
  for (const auto& file : files) std::ofstream(root / file.first) << file.second;
  return root;
}

void validRegistry() {
  Registry registry;
  assert(registry.load(REGISTRY_DATA_DIR));
  assert(registry.steamItemId() == ItemId::pack("1111:11:1"));
  assert(registry.pipe(ItemId::pack("1111:10:0")) != nullptr);
  assert(registry.cable(ItemId::pack("1111:01:0")) != nullptr);
  assert(registry.fluid(2)->item_id == registry.steamItemId());
  assert(registry.drops().size() == 2);
}

void rejectsDuplicateAndUnknown() {
  const auto root = tempRegistry(
      "invalid", "int,name,stack,meta\n0:0:0,air,,0\n0:0:0,air_again,,0\n0:0:1,air,,0\n",
      "item_id,name,tier,flow_rate,items_per_sec\n0:0:9,pipe,0,1,0\n",
      "item_id,name,tier,loss,ampacity,insulation\n0:0:0,wrong,0,1,1\n",
      "fluid_id,item_id,name,color,density,gaseous,temperature\n2,0:0:0,steam,fff,1,true,300\n",
      "source,result\n0:0:9,0:0:0\n");
  Registry registry;
  assert(!registry.load(root.string()));
  assert(!registry.errors().empty());
  std::filesystem::remove_all(root);
}

void rejectsMismatchedName() {
  const auto root = tempRegistry(
      "mismatch", "int,name,stack,meta\n0:0:0,air,,0\n",
      "item_id,name,tier,flow_rate,items_per_sec\n0:0:0,not_air,0,1,0\n",
      "item_id,name,tier,loss,ampacity,insulation\n",
      "fluid_id,item_id,name,color,density,gaseous,temperature\n",
      "source,result\n");
  Registry registry;
  assert(!registry.load(root.string()));
  std::filesystem::remove_all(root);
}
} // namespace

int main() {
  validRegistry();
  rejectsDuplicateAndUnknown();
  rejectsMismatchedName();
}
