#include <common/Registry.h>

#include <cassert>
#include <cstdint>
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
  const auto* steam_fluid = registry.fluid(registry.steamItemId());
  (void)steam_fluid;
  assert(steam_fluid != nullptr && steam_fluid->item_id == registry.steamItemId());
  assert(registry.drops().size() == 2);
}

// Task 1.4.4: the resolved Steam ID must equal the items.csv row AND the
// fluids.csv mapping for the same object.
void steamIdMatchesRegistryData() {
  Registry registry;
  assert(registry.load(REGISTRY_DATA_DIR));
  const std::uint16_t steam = registry.steamItemId();
  assert(steam == ItemId::pack("1111:11:1"));
  const auto* item = registry.item(steam);
  (void)item;
  assert(item != nullptr && item->name == "steam");
  assert(item->id_text == "1111:11:1");
  const auto* fluid = registry.fluid(steam);
  (void)fluid;
  assert(fluid != nullptr && fluid->item_id == steam);
  assert(fluid->name == "steam");
  assert(gtnh::common::steamItemId() == steam);
}

// Without a fluids.csv property row the accessor fails closed: steam exists as
// an item, but no steam identity may be handed out for fluid operations.
void steamIdFailsClosedWithoutFluidMapping() {
  const auto root = tempRegistry(
      "steam-unmapped",
      "int,name,stack,meta\n1111:11:0,water,,0\n1111:11:1,steam,,0\n",
      "item_id,name,tier,flow_rate,items_per_sec\n",
      "item_id,name,tier,loss,ampacity,insulation\n",
      "item_id,name,color,density,gaseous,temperature\n1111:11:0,water,1e5bc9,1000,false,300\n",
      "");
  Registry registry;
  assert(registry.load(root.string()));
  assert(registry.itemByName("steam") != nullptr);
  assert(registry.steamItemId() == 0);
  std::filesystem::remove_all(root);
}

void rejectsUnknownAndDuplicateFluidItemIds() {
  const auto unknown = tempRegistry(
      "fluid-unknown", "int,name,stack,meta\n0:0:0,air,,0\n",
      "item_id,name,tier,flow_rate,items_per_sec\n",
      "item_id,name,tier,loss,ampacity,insulation\n",
      "item_id,name,color,density,gaseous,temperature\n1111:11:9,ghost,ffffff,1,false,300\n",
      "source,result\n");
  Registry unknown_registry;
  assert(!unknown_registry.load(unknown.string()));
  assert(!unknown_registry.errors().empty());
  std::filesystem::remove_all(unknown);

  const auto duplicate = tempRegistry(
      "fluid-duplicate", "int,name,stack,meta\n1111:11:1,steam,,0\n",
      "item_id,name,tier,flow_rate,items_per_sec\n",
      "item_id,name,tier,loss,ampacity,insulation\n",
      "item_id,name,color,density,gaseous,temperature\n"
      "1111:11:1,steam,e0e0e0,0.6,true,400\n"
      "1111:11:1,steam,e0e0e0,0.6,true,500\n",
      "");
  Registry duplicate_registry;
  assert(!duplicate_registry.load(duplicate.string()));
  assert(!duplicate_registry.errors().empty());
  std::filesystem::remove_all(duplicate);
}

void rejectsDuplicateAndUnknown() {
  const auto root = tempRegistry(
      "invalid", "int,name,stack,meta\n0:0:0,air,,0\n0:0:0,air_again,,0\n0:0:1,air,,0\n",
      "item_id,name,tier,flow_rate,items_per_sec\n0:0:9,pipe,0,1,0\n",
      "item_id,name,tier,loss,ampacity,insulation\n0:0:0,wrong,0,1,1\n",
      "item_id,name,color,density,gaseous,temperature\n0:0:0,steam,fff,1,true,300\n",
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
      "item_id,name,color,density,gaseous,temperature\n",
      "source,result\n");
  Registry registry;
  assert(!registry.load(root.string()));
  std::filesystem::remove_all(root);
}
} // namespace

int main() {
  validRegistry();
  steamIdMatchesRegistryData();
  steamIdFailsClosedWithoutFluidMapping();
  rejectsUnknownAndDuplicateFluidItemIds();
  rejectsDuplicateAndUnknown();
  rejectsMismatchedName();
}
