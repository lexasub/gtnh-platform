#pragma once

#include "ConditionEvaluator.h"
#include "RecipeConditions.h"
#include "RecipeTypes.h"
#include "recipe_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
namespace YAML {
    class Node;
}
namespace gtnh {
namespace common {
class Registry;
} // namespace common
} // namespace gtnh

namespace RecipeManager {

/// Energy types used in machine variant definitions
enum class EnergyType : uint8_t {
  ELECTRICITY = 0,
  HEAT = 1,
  STEAM = 2,
  ROTATION = 3,
};

/// Per-variant multiplier that scales base recipe values
struct VariantMultiplier {
  double duration = 1.0;
  double output = 1.0;
  double eu = 1.0;
};

/// A single machine variant (block instance) within a machine class
struct MachineVariant {
  uint16_t block_id;
  std::string name;
  EnergyType
      energy_in; // what the machine consumes (NONE = burns fuel directly)
  EnergyType energy_out; // what the machine produces (NONE = consumer)
  int16_t tier;
};

/// Loaded from machines.yaml
struct MachineClassDef {
  std::string name;
  std::vector<MachineVariant> variants;
};

class RecipeManager {
public:
  RecipeManager();
  ~RecipeManager();

  // ── YAML loaders ────────────────────────────────────────────────
  /// Load machines.yaml: builds class/variant maps used for recipe
  /// tier filtering and block_id → class resolution.
  bool loadMachinesFromYaml(const std::string &filePath);

  /// Load a YAML recipe file: recipes for a single machine class.
  /// Expects format: { class: "macerator", recipes: [...] }
  bool loadRecipesFromYamlFile(const std::string &filePath);

  /// Load all .yaml recipe files from a directory.
  bool loadRecipesFromYamlDirectory(const std::string &directoryPath);

  /// Runtime registration of a block_id → machine class mapping (used for
  /// multiblock controllers whose blocks are not yet in machines.yaml).
  /// Energy_in: ENERGY_TYPE_ANY (255) = any.
  void registerMachineClass(uint16_t block_id, const std::string &class_name,
                            int16_t tier = 0, uint8_t energy_in = 255);

  // ── RPC handlers ────────────────────────────────────────────────
  std::string checkRecipe(const Protocol::Container *container,
                          uint16_t machine_id);
  std::unique_ptr<Protocol::Container>
  craft(const std::string &recipeId, const Protocol::Container *container);
  bool evaluateConditions(const std::string &recipeId);
  bool evaluateConditions(const std::string &recipeId,
                          const MachineState &state) const;

  std::vector<uint8_t>
  handleCheckRecipeRequest(const Protocol::CheckRecipeReq *request,
                           uint32_t req_id);
  std::vector<uint8_t> handleCraftRequest(const Protocol::CraftReq *request,
                                          uint32_t req_id);
  std::vector<uint8_t> handleEvaluateConditionsRequest(
      const Protocol::EvaluateConditionsReq *request, uint32_t req_id);
  // Client-driven recipe queries (catalog + per-item + per-machine).
  std::vector<uint8_t> handleCatalogRequest(uint32_t req_id);
  std::vector<uint8_t> handleRecipesForItemRequest(
      const Protocol::RecipesForItemReq *request, uint32_t req_id);
  std::vector<uint8_t> handleRecipesForMachineRequest(
      const Protocol::RecipesForMachineReq *request, uint32_t req_id);

  // ── Public accessors ────────────────────────────────────────────
  size_t recipeCount() const { return recipes_.size(); }
  const Recipe *getRecipeById(const std::string &id) const;
  const Recipe *findRecipeByInputs(uint16_t machine_id,
                                   const std::vector<ItemStack> &inputs) const;

  /// Compact catalog: every item id appearing as a recipe input or output
  /// (deduped, insertion order). "What recipes exist" for the client.
  std::vector<uint16_t> collectRecipeItemIds() const;

  /// Recipes involving `item_id`. mode: 0 = both, 1 = craft (item is an
  /// output), 2 = use (item is an input).
  std::vector<const Recipe *> findRecipesForItem(uint16_t item_id,
                                                 uint8_t mode) const;

  /// All recipes the machine class of `machine_id` can run (NEI-style
  /// listing). Empty if the block_id is not a known machine.
  std::vector<const Recipe *> findRecipesForMachine(uint16_t machine_id) const;

  /// Serialize a Recipe into Protocol::RecipeInfo (inputs/outputs as ItemStack
  /// vectors, optional positional pattern). Used by the client-query handlers.
  static flatbuffers::Offset<Protocol::RecipeInfo>
  buildRecipeInfo(flatbuffers::FlatBufferBuilder &builder, const Recipe &recipe);

  /// Get the tier of a machine variant by block_id.
  /// Returns 0 if not found (safe default for legacy machines).
  int16_t getMachineTier(uint16_t block_id) const;

  /// Get the energy_in type of a machine variant by block_id.
  /// Returns ENERGY_TYPE_ANY (255) if not found.
  uint8_t getMachineEnergyIn(uint16_t block_id) const;

  /// Get machine class name for a block_id (empty string if not found).
  const std::string &getMachineClass(uint16_t block_id) const;

  /// Validate every stored recipe's resource requirements (4.1.3): kind/ID
  /// against the item registry, tier against the recipe's machine class, and
  /// — when a loaded shared CSV registry is provided — FLUID ids against its
  /// canonical fluid catalog (steam must resolve to Registry::steamItemId()).
  /// Returns one message per violation; empty = all recipes valid.
  std::vector<std::string>
  validateResourceRequirements(const ::gtnh::common::Registry *shared) const;

private:
  // ── Recipe storage ──────────────────────────────────────────────
  std::unordered_map<std::string, Recipe> recipes_;
  std::unordered_map<std::string, std::vector<std::string>>
      recipesByClass_; // class → recipe IDs

  // ── Machine registry (from YAML) ────────────────────────────────
  std::unordered_map<std::string, MachineClassDef> classes_;
  std::unordered_map<uint16_t, std::string>
      classByBlockId_;                                  // block_id → class name
  std::unordered_map<uint16_t, int16_t> tierByBlockId_; // block_id → tier
  std::unordered_map<uint16_t, uint8_t>
      energyInByBlockId_;   // block_id → energy_in (255 = ANY)
  std::string emptyString_; // safe empty return

  // ── YAML parsers ────────────────────────────────────────────────
  EnergyType parseEnergyType(const std::string &str) const;
  bool parseYamlMachines(const YAML::Node &root);
  bool parseYamlMachineClass(const YAML::Node &node);
  bool parseYamlRecipes(const YAML::Node &root,
                        const std::string &defaultClass);
  bool parseYamlRecipe(const YAML::Node &yaml, const std::string &defaultClass);
  InputItem parseYamlInputItem(const YAML::Node &node);
  OutputItem parseYamlOutputItem(const YAML::Node &node);
  void parseYamlConditions(const YAML::Node &node,
                           RecipeConditions &conditions);

  /// Resolve item name to ID via ItemRegistry. Returns 0 if unknown.
  uint16_t resolveItemName(const std::string &name) const;

  /// Format-detect an `item:` scalar: hierarchical (contains ':'),
  /// flat numeric (all digits), else string name. Returns packed uint16_t.
  uint16_t resolveItemId(const std::string &itemStr) const;

  /// gp-hmb0: how one `item:` scalar was resolved by resolveItemId.
  ///
  /// The three-way split exists because the packed id CANNOT express the
  /// difference: id 0 is both the "unresolved" return value and the real id
  /// of `air` (0:0:0 in registry/items.csv), so a caller testing `id == 0`
  /// would have to guess. Note that only the NAME branch can fail —
  /// a hierarchical or all-digits scalar is packed arithmetically and is
  /// never looked up, exactly as before this change.
  enum class ItemResolution : uint8_t {
    Name,        // resolved through the registry (may legitimately be air/0)
    Literal,     // hierarchical or flat-numeric scalar, packed arithmetically
    Unresolved,  // a NAME that is not in the registry — rejected at load time
  };

  /// Resolve an `item:` scalar AND report whether it was found.
  ///
  /// This is the single entry point the YAML parsers use. The key point is
  /// that the format detection is shared with resolveItemId (see
  /// isNameForm), so a name is only ever sent to the registry, and only a
  /// name can come back Unresolved. `outId` is the same value resolveItemId
  /// would have returned, so numeric/packed ids are bit-for-bit unaffected.
  ItemResolution resolveItemIdChecked(const std::string &itemStr,
                                      uint16_t &outId) const;

  /// True when the scalar is a NAME, i.e. the only form that requires a
  /// registry lookup: no ':' and at least one non-digit character. This is
  /// the exact classification resolveItemId performs, factored out so the
  /// checked and unchecked paths cannot drift apart.
  static bool isNameForm(const std::string &itemStr);

  /// gp-hmb0: the first `item:` (or `replace:`) scalar in `node` that is a
  /// NAME the item registry does not know, or "" when the node is clean.
  /// The load-time rejection predicate: a non-empty return means the recipe
  /// must be rejected, because the scalar would otherwise resolve to 0.
  std::string firstUnresolvedItemName(const YAML::Node &node) const;

  // ── Helpers ──────────────────────────────────────────────────────
  std::vector<ItemStack>
  convertContainerItems(const Protocol::Container *container) const;
  std::unique_ptr<Protocol::Container>
  createContainer(const std::vector<ItemStack> &items) const;
};

} // namespace RecipeManager
