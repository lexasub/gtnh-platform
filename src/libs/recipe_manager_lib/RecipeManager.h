#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <optional>
#include <flatbuffers/flatbuffers.h>
#include "recipe_generated.h"
#include <nlohmann/json.hpp>
#include "RecipeTypes.h"
#include "RecipeConditions.h"
#include "ConditionEvaluator.h"

namespace RecipeManager {

class RecipeManager {
public:
    RecipeManager();
    ~RecipeManager();

    bool loadRecipesFromDirectory(const std::string& directoryPath);

    // Parse a single compact JSON file (object keyed by recipe_id)
    bool parseRecipeFile(const nlohmann::json& root);

    // Parse one recipe value from compact format:
    //   "recipe_id": {"m": N, "in": [[id, cnt?], ...], "out": [[id, cnt?, {override?}]], "dur": N, "eu": N}
    bool parseCompactRecipe(const std::string& recipeId, const nlohmann::json& data);

    // Legacy parser for old format (array of {id, input, output, machine, duration, energy_cost})
    bool parseLegacyRecipeJson(const nlohmann::json& recipeData);

    // Parsers for compact array notation
    std::vector<InputItem> parseCompactInputList(const nlohmann::json& arr);
    std::vector<OutputItem> parseCompactOutputList(const nlohmann::json& arr);
    OutputItem parseCompactOutputEntry(const nlohmann::json& entry);
    OutputItem parseOutputOverride(uint16_t itemId, uint8_t count, const nlohmann::json& overrideObj);

    // Legacy parsers (string-based JSON format)
    void parseLegacyItemStack(const nlohmann::json& data, std::vector<InputItem>& items);
    void parseLegacyItemStackArray(const nlohmann::json& data, std::vector<InputItem>& items);

    uint16_t stringToMachineId(const std::string& str);

    // RPC handlers
    std::string checkRecipe(const Protocol::Container* container, uint16_t machine_id);
    std::unique_ptr<Protocol::Container> craft(const std::string& recipeId, const Protocol::Container* container);
    bool evaluateConditions(const std::string& recipeId);
    bool evaluateConditions(const std::string& recipeId, const MachineState& state) const;

    // FlatBuffers message handlers
    std::vector<uint8_t> handleCheckRecipeRequest(const Protocol::CheckRecipeReq* request, uint32_t req_id);
    std::vector<uint8_t> handleCraftRequest(const Protocol::CraftReq* request, uint32_t req_id);
    std::vector<uint8_t> handleEvaluateConditionsRequest(const Protocol::EvaluateConditionsReq* request, uint32_t req_id);

    // Public accessor
    size_t recipeCount() const { return recipes_.size(); }

    // Lookup by ID
    const Recipe* getRecipeById(const std::string& id) const;

    // Lookup by machine_id and inputs
    const Recipe* findRecipeByInputs(uint16_t machine_id, const std::vector<ItemStack>& inputs) const;

private:
    std::unordered_map<std::string, Recipe> recipes_;
    std::unordered_map<uint16_t, std::vector<std::string>> recipesByMachineId_;

    std::vector<ItemStack> convertContainerItems(const Protocol::Container* container) const;
    std::unique_ptr<Protocol::Container> createContainer(const std::vector<ItemStack>& items) const;
};

} // namespace RecipeManager
