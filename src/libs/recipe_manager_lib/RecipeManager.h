#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <optional>
#include <flatbuffers/flatbuffers.h>
#include "recipe_generated.h"
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include "RecipeTypes.h"
#include "RecipeConditions.h"
#include "ConditionEvaluator.h"

namespace RecipeManager {

/// Energy types used in machine variant definitions
enum class EnergyType : uint8_t {
    ELECTRICITY = 0,
    HEAT = 1,
    STEAM = 2,
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
    EnergyType energy_in;   // what the machine consumes (NONE = burns fuel directly)
    EnergyType energy_out;  // what the machine produces (NONE = consumer)
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

    // ── JSON loaders (existing) ──────────────────────────────────────
    bool loadRecipesFromDirectory(const std::string& directoryPath);
    bool parseRecipeFile(const nlohmann::json& root);
    bool parseCompactRecipe(const std::string& recipeId, const nlohmann::json& data);
    bool parseLegacyRecipeJson(const nlohmann::json& recipeData);
    std::vector<InputItem> parseCompactInputList(const nlohmann::json& arr);
    std::vector<OutputItem> parseCompactOutputList(const nlohmann::json& arr);
    OutputItem parseCompactOutputEntry(const nlohmann::json& entry);
    OutputItem parseOutputOverride(uint16_t itemId, uint8_t count, const nlohmann::json& overrideObj);
    void parseLegacyItemStack(const nlohmann::json& data, std::vector<InputItem>& items);
    void parseLegacyItemStackArray(const nlohmann::json& data, std::vector<InputItem>& items);
    uint16_t stringToMachineId(const std::string& str);

    // ── YAML loaders (new) ──────────────────────────────────────────
    /// Load machines.yaml: builds class/variant maps used for recipe
    /// tier filtering and block_id → class resolution.
    bool loadMachinesFromYaml(const std::string& filePath);

    /// Load a YAML recipe file: recipes for a single machine class.
    /// Expects format: { class: "macerator", recipes: [...] }
    bool loadRecipesFromYamlFile(const std::string& filePath);

    /// Load all .yaml recipe files from a directory.
    bool loadRecipesFromYamlDirectory(const std::string& directoryPath);

    // ── RPC handlers ────────────────────────────────────────────────
    std::string checkRecipe(const Protocol::Container* container, uint16_t machine_id);
    std::unique_ptr<Protocol::Container> craft(const std::string& recipeId, const Protocol::Container* container);
    bool evaluateConditions(const std::string& recipeId);
    bool evaluateConditions(const std::string& recipeId, const MachineState& state) const;

    std::vector<uint8_t> handleCheckRecipeRequest(const Protocol::CheckRecipeReq* request, uint32_t req_id);
    std::vector<uint8_t> handleCraftRequest(const Protocol::CraftReq* request, uint32_t req_id);
    std::vector<uint8_t> handleEvaluateConditionsRequest(const Protocol::EvaluateConditionsReq* request, uint32_t req_id);

    // ── Public accessors ────────────────────────────────────────────
    size_t recipeCount() const { return recipes_.size(); }
    const Recipe* getRecipeById(const std::string& id) const;
    const Recipe* findRecipeByInputs(uint16_t machine_id, const std::vector<ItemStack>& inputs) const;

    /// Get the tier of a machine variant by block_id.
    /// Returns 0 if not found (safe default for legacy machines).
    int16_t getMachineTier(uint16_t block_id) const;

    /// Get the energy_in type of a machine variant by block_id.
    /// Returns ENERGY_TYPE_ANY (255) if not found.
    uint8_t getMachineEnergyIn(uint16_t block_id) const;

    /// Get machine class name for a block_id (empty string if not found).
    const std::string& getMachineClass(uint16_t block_id) const;

private:
    // ── Recipe storage ──────────────────────────────────────────────
    std::unordered_map<std::string, Recipe> recipes_;
    std::unordered_map<uint16_t, std::vector<std::string>> recipesByMachineId_;  // legacy: block_id → recipe IDs
    std::unordered_map<std::string, std::vector<std::string>> recipesByClass_;    // new: class → recipe IDs

    // ── Machine registry (from YAML) ────────────────────────────────
    std::unordered_map<std::string, MachineClassDef> classes_;
    std::unordered_map<uint16_t, std::string> classByBlockId_;        // block_id → class name
    std::unordered_map<uint16_t, int16_t> tierByBlockId_;             // block_id → tier
    std::unordered_map<uint16_t, uint8_t> energyInByBlockId_;         // block_id → energy_in (255 = ANY)
    std::string emptyString_;                                         // safe empty return

    // ── YAML parsers ────────────────────────────────────────────────
    EnergyType parseEnergyType(const std::string& str) const;
    bool parseYamlMachines(const YAML::Node& root);
    bool parseYamlMachineClass(const YAML::Node& node);
    bool parseYamlRecipes(const YAML::Node& root, const std::string& defaultClass);
    bool parseYamlRecipe(const YAML::Node& yaml, const std::string& defaultClass);
    InputItem parseYamlInputItem(const YAML::Node& node);
    OutputItem parseYamlOutputItem(const YAML::Node& node);
    void parseYamlConditions(const YAML::Node& node, RecipeConditions& conditions);

    /// Resolve item name to ID via ItemRegistry. Returns 0 if unknown.
    uint16_t resolveItemName(const std::string& name) const;

    // ── Helpers ──────────────────────────────────────────────────────
    std::vector<ItemStack> convertContainerItems(const Protocol::Container* container) const;
    std::unique_ptr<Protocol::Container> createContainer(const std::vector<ItemStack>& items) const;
};

} // namespace RecipeManager
