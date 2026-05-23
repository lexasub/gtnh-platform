#include "RecipeManager.h"
#include "ItemRegistry.h"
#include "ConditionEvaluator.h"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>

using json = nlohmann::json;

namespace RecipeManager {

// ---------------------------------------------------------------------------
// Recipe matching & crafting
// ---------------------------------------------------------------------------

bool Recipe::matches(const std::vector<ItemStack>& container_items) const {
    // Quick check: we need at least as many non-empty container slots as non-empty inputs.
    // Empty slots in the recipe (item_id==0) are just filler — they don't count as required.
    size_t requiredInputs = 0;
    for (const auto& req : inputs) {
        if (req.item_id != 0) ++requiredInputs;
    }
    size_t available = 0;
    for (const auto& slot : container_items) {
        if (slot.item_id != 0 && slot.count > 0) ++available;
    }
    if (available < requiredInputs) return false;

    // For each required (non-empty) input, find a matching container slot.
    // consume=false items (e.g., buckets) are not consumed — skip them in matching.
    std::vector<bool> used(container_items.size(), false);

    for (const auto& req : inputs) {
        if (req.item_id == 0) continue;  // Empty recipe slot — no item required
        bool found = false;
        for (size_t i = 0; i < container_items.size(); ++i) {
            if (used[i]) continue;
            const auto& slot = container_items[i];
            // Must match item_id, have enough count, and match metadata
            if (slot.item_id == req.item_id &&
                slot.count >= req.count &&
                slot.metadata == req.metadata)
            {
                used[i] = true;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    return true;
}

std::vector<ItemStack> Recipe::craft(const std::vector<ItemStack>& container_items) const {
    // Copy container — we'll modify it
    std::vector<ItemStack> result = container_items;

    // Consume inputs (skip items with consume=false)
    for (const auto& req : inputs) {
        if (!req.consume) {
            if (req.replace_item > 0) {
                for (auto& slot : result) {
                    if (slot.item_id == req.item_id && slot.metadata == req.metadata) {
                        slot.item_id = req.replace_item;
                        slot.metadata = req.replace_meta;
                        break;
                    }
                }
            }
            continue;
        }

        int64_t remaining = req.count;
        for (auto& slot : result) {
            if (remaining <= 0) break;
            if (slot.item_id == req.item_id && slot.metadata == req.metadata) {
                uint8_t take = static_cast<uint8_t>(std::min(static_cast<int64_t>(slot.count), remaining));
                slot.count -= take;
                remaining -= take;
                if (slot.count == 0) {
                    slot.item_id = 0;
                    slot.metadata = 0;
                }
            }
        }
    }

    // Find first empty slot and add outputs
    for (const auto& out : outputs) {
        // Try to stack with existing matching slot first
        bool stacked = false;
        for (auto& slot : result) {
            if (slot.item_id == out.item_id && slot.metadata == out.metadata) {
                uint8_t space = 64 - slot.count; // max stack = 64 for now
                if (space >= out.count) {
                    slot.count += out.count;
                    stacked = true;
                    break;
                }
            }
        }
        if (stacked) continue;

        // Find empty slot
        bool placed = false;
        for (auto& slot : result) {
            if (slot.item_id == 0) {
                slot.item_id = out.item_id;
                slot.count = out.count;
                slot.metadata = out.metadata;
                placed = true;
                break;
            }
        }
        if (!placed) {
            spdlog::warn("No empty slot for output {} in recipe {}", out.item_id, id);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RecipeManager::RecipeManager() {
    spdlog::info("RecipeManager constructed");
}

RecipeManager::~RecipeManager() {
    spdlog::info("RecipeManager destroyed");
}

// ---------------------------------------------------------------------------
// FlatBuffers <-> internal conversion
// ---------------------------------------------------------------------------

std::vector<ItemStack> RecipeManager::convertContainerItems(const Protocol::Container* container) const {
    std::vector<ItemStack> items;
    auto itemArr = container->items();
    for (uint16_t i = 0; i < 9; ++i) {
        const auto* stack = itemArr->Get(i);
        if (stack) {
            items.push_back({
                static_cast<uint16_t>(stack->item_id()),
                static_cast<uint8_t>(stack->count()),
                stack->meta()
            });
        } else {
            items.push_back({0, 0, 0});
        }
    }
    return items;
}

std::unique_ptr<Protocol::Container> RecipeManager::createContainer(const std::vector<ItemStack>& items) const {
    std::array<Protocol::ItemStack, 9> itemsArray{};
    for (size_t i = 0; i < std::min(items.size(), static_cast<size_t>(9)); ++i) {
        const auto& itm = items[i];
        itemsArray[i] = Protocol::ItemStack(itm.item_id, itm.count, itm.metadata);
    }
    uint16_t size = 0;
    for (const auto& itm : itemsArray) {
        if (itm.item_id() != 0) ++size;
    }
    return std::make_unique<Protocol::Container>(
        flatbuffers::span<const Protocol::ItemStack, 9>(itemsArray), 0, size);
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

bool RecipeManager::loadRecipesFromDirectory(const std::string& directoryPath) {
    spdlog::info("Loading recipes from directory: {}", directoryPath);

    try {
        std::vector<std::string> recipeFiles;

        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                recipeFiles.push_back(entry.path().string());
            }
        }

        if (recipeFiles.empty()) {
            spdlog::warn("No JSON recipe files found in directory: {}", directoryPath);
            return true;
        }

        size_t totalLoaded = 0;

        for (const auto& filePath : recipeFiles) {
            std::ifstream file(filePath);
            if (!file.is_open()) {
                spdlog::warn("Failed to open recipe file: {}", filePath);
                continue;
            }

            json data;
            try {
                file >> data;
            } catch (const json::parse_error& e) {
                spdlog::warn("Failed to parse JSON in file {}: {}", filePath, e.what());
                continue;
            }

            size_t fileLoaded = 0;

            if (data.is_object()) {
                bool compact = false;
                for (auto it = data.begin(); it != data.end(); ++it) {
                    if (it->is_object() && it->contains("m")) {
                        compact = true;
                    }
                    break;
                }

                if (compact) {
                    fileLoaded = parseRecipeFile(data) ? data.size() : 0;
                } else {
                    fileLoaded = 0;
                    for (auto it = data.begin(); it != data.end(); ++it) {
                        if (it->is_array()) {
                            for (const auto& recipeData : *it) {
                                if (parseLegacyRecipeJson(recipeData)) fileLoaded++;
                            }
                        }
                    }
                    if (fileLoaded == 0) {
                        fileLoaded = parseLegacyRecipeJson(data) ? 1 : 0;
                    }
                }
            } else if (data.is_array()) {
                for (const auto& recipeData : data) {
                    if (parseLegacyRecipeJson(recipeData)) fileLoaded++;
                }
            }

            spdlog::info("Loaded {} recipes from {}", fileLoaded, filePath);
            totalLoaded += fileLoaded;
        }

        spdlog::info("Loaded {} total recipes from directory", totalLoaded);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("Error loading recipes from directory {}: {}", directoryPath, e.what());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Compact format parser
// ---------------------------------------------------------------------------

bool RecipeManager::parseRecipeFile(const json& root) {
    if (!root.is_object()) return false;
    bool anyOk = false;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (parseCompactRecipe(it.key(), it.value())) anyOk = true;
    }
    return anyOk;
}

bool RecipeManager::parseCompactRecipe(const std::string& recipeId, const json& data) {
    try {
        if (!data.is_object()) {
            spdlog::warn("Recipe '{}': value is not an object", recipeId);
            return false;
        }

        Recipe recipe;
        recipe.id = recipeId;

        recipe.machine_id = data.value("m", 0);

        if (data.contains("in") && data["in"].is_array()) {
            recipe.inputs = parseCompactInputList(data["in"]);
        }

        if (!data.contains("out") || !data["out"].is_array()) {
            spdlog::warn("Recipe '{}': missing or invalid 'out' field", recipeId);
            return false;
        }
        recipe.outputs = parseCompactOutputList(data["out"]);
        if (recipe.outputs.empty()) {
            spdlog::warn("Recipe '{}': empty output list", recipeId);
            return false;
        }

        if (data.contains("dur")) {
            recipe.duration = data["dur"].get<uint32_t>();
        } else {
            recipe.duration = 200;
        }
        if (recipe.duration == 0) recipe.duration = 1;

        if (data.contains("eu")) {
            recipe.energy_cost = data["eu"].get<float>();
        } else {
            recipe.energy_cost = 0.0f;
        }

        if (data.contains("env") && data["env"].is_object()) {
            auto& env = data["env"];
            if (env.contains("temp") && env["temp"].is_object()) {
                auto& temp = env["temp"];
                recipe.conditions.environment.emplace();
                recipe.conditions.environment->temperature.emplace();
                if (temp.contains("min")) {
                    recipe.conditions.environment->temperature->min = temp["min"].get<float>();
                }
                if (temp.contains("max")) {
                    recipe.conditions.environment->temperature->max = temp["max"].get<float>();
                }
            }
            if (env.contains("purity")) {
                recipe.conditions.environment->purity = env["purity"].get<float>();
            }
            if (env.contains("biomes") && env["biomes"].is_array()) {
                for (const auto& b : env["biomes"]) {
                    if (b.is_number()) {
                        recipe.conditions.environment->biomes.push_back(b.get<uint16_t>());
                    }
                }
            }
        }
        
        if (data.contains("mach") && data["mach"].is_object()) {
            auto& mach = data["mach"];
            recipe.conditions.machine.emplace();
            if (mach.contains("energy") && mach["energy"].is_object()) {
                auto& energy = mach["energy"];
                if (energy.contains("min")) {
                    recipe.conditions.machine->energy_min = energy["min"].get<uint32_t>();
                }
                if (energy.contains("max")) {
                    recipe.conditions.machine->energy_max = energy["max"].get<uint32_t>();
                }
            }
            if (mach.contains("network_id")) {
                recipe.conditions.machine->network_id = mach["network_id"].get<uint32_t>();
            }
            if (mach.contains("facing")) {
                recipe.conditions.machine->facing = mach["facing"].get<uint8_t>();
            }
        }
        
        if (data.contains("spec") && data["spec"].is_array()) {
            for (const auto& specElem : data["spec"]) {
                if (specElem.is_array() && specElem.size() >= 3) {
                    SpecialCondition sc;
                    sc.key = specElem[0].get<uint16_t>();
                    sc.value_type = specElem[1].get<uint8_t>();
                    switch (sc.value_type) {
                        case 0:
                            sc.int_value = specElem[2].get<int32_t>();
                            break;
                        case 1:
                            sc.float_value = specElem[2].get<float>();
                            break;
                        case 2:
                            sc.string_value = specElem[2].get<std::string>();
                            break;
                        default:
                            continue;
                    }
                    recipe.conditions.special.push_back(std::move(sc));
                }
            }
        }

        recipes_[recipe.id] = recipe;
        recipesByMachineId_[recipe.machine_id].push_back(recipe.id);

        return true;

    } catch (const std::exception& e) {
        spdlog::warn("Error parsing recipe '{}': {}", recipeId, e.what());
        return false;
    }
}

std::vector<InputItem> RecipeManager::parseCompactInputList(const json& arr) {
    std::vector<InputItem> result;
    for (const auto& elem : arr) {
        if (!elem.is_array() || elem.empty()) continue;

        uint16_t item_id      = elem[0].get<uint16_t>();
        uint8_t  count        = (elem.size() >= 2) ? elem[1].get<uint8_t>() : 1;
        bool     consume      = true;
        uint16_t replace_item = 0;
        uint16_t replace_meta = 0;

        if (elem.size() >= 3 && elem[2].is_object()) {
            consume      = elem[2].value("consume", true);
            replace_item = elem[2].value("replace", 0);
            replace_meta = elem[2].value("replace_meta", 0);
        }

        result.push_back({item_id, count, 0, consume, replace_item, replace_meta});
    }
    return result;
}

std::vector<OutputItem> RecipeManager::parseCompactOutputList(const json& arr) {
    std::vector<OutputItem> result;
    for (const auto& elem : arr) {
        if (!elem.is_array() || elem.empty()) continue;

        uint16_t item_id = elem[0].get<uint16_t>();
        uint8_t  count   = (elem.size() >= 2) ? elem[1].get<uint8_t>() : 1;

        if (elem.size() >= 3 && elem[2].is_object()) {
            result.push_back(parseOutputOverride(item_id, count, elem[2]));
        } else {
            result.push_back({item_id, count, 0, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
        }
    }
    return result;
}

OutputItem RecipeManager::parseOutputOverride(uint16_t itemId, uint8_t count, const json& obj) {
    OutputItem out;
    out.item_id  = itemId;
    out.count    = count;
    out.metadata = obj.value("meta", 0);

    if (obj.contains("display_name") && obj["display_name"].is_string())
        out.display_name = obj["display_name"].get<std::string>();

    if (obj.contains("nbt") && obj["nbt"].is_object())
        out.nbt = obj["nbt"];

    if (obj.contains("color") && obj["color"].is_string())
        out.color = obj["color"].get<std::string>();

    if (obj.contains("lore") && obj["lore"].is_array()) {
        std::vector<std::string> loreVec;
        for (const auto& l : obj["lore"]) {
            if (l.is_string()) loreVec.push_back(l.get<std::string>());
        }
        if (!loreVec.empty()) out.lore = loreVec;
    }

    if (obj.contains("unlocalized_name") && obj["unlocalized_name"].is_string())
        out.unlocalized_name = obj["unlocalized_name"].get<std::string>();

    return out;
}

// ---------------------------------------------------------------------------
// Legacy parsers
// ---------------------------------------------------------------------------

bool RecipeManager::parseLegacyRecipeJson(const json& recipeData) {
    try {
        if (!recipeData.is_object()) return false;

        Recipe recipe;

        if (recipeData.contains("id") && recipeData["id"].is_string()) {
            recipe.id = recipeData["id"];
        } else if (recipeData.contains("description") &&
                   recipeData["description"].contains("identifier") &&
                   recipeData["description"]["identifier"].is_string()) {
            recipe.id = recipeData["description"]["identifier"];
        } else {
            return false;
        }

        if (recipeData.contains("input")) {
            parseLegacyItemStack(recipeData["input"], recipe.inputs);
        } else if (recipeData.contains("inputs")) {
            parseLegacyItemStackArray(recipeData["inputs"], recipe.inputs);
        }

        if (recipeData.contains("output")) {
            std::vector<InputItem> tmp;
            parseLegacyItemStack(recipeData["output"], tmp);
            for (auto& s : tmp) {
                recipe.outputs.push_back({s.item_id, s.count, s.metadata,
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
            }
        } else if (recipeData.contains("outputs")) {
            std::vector<InputItem> tmp;
            parseLegacyItemStackArray(recipeData["outputs"], tmp);
            for (auto& s : tmp) {
                recipe.outputs.push_back({s.item_id, s.count, s.metadata,
                    std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
            }
        } else {
            return false;
        }

        if (recipe.outputs.empty()) return false;

        if (recipeData.contains("machine")) {
            if (recipeData["machine"].is_string()) {
                recipe.machine_id = stringToMachineId(recipeData["machine"].get<std::string>());
            } else if (recipeData["machine"].is_number()) {
                recipe.machine_id = recipeData["machine"].get<uint16_t>();
            } else {
                recipe.machine_id = 0;
            }
        } else {
            recipe.machine_id = 0;
        }

        if (recipeData.contains("duration")) {
            recipe.duration = recipeData["duration"].get<uint32_t>();
        } else if (recipeData.contains("cookingtime")) {
            recipe.duration = recipeData["cookingtime"].get<uint32_t>();
        } else {
            recipe.duration = 200;
        }
        if (recipe.duration == 0) recipe.duration = 1;

        if (recipeData.contains("energy_cost")) {
            recipe.energy_cost = recipeData["energy_cost"].get<float>();
        } else if (recipeData.contains("experience")) {
            recipe.energy_cost = recipeData["experience"].get<float>();
        } else {
            recipe.energy_cost = 0.0f;
        }

        recipes_[recipe.id] = recipe;
        recipesByMachineId_[recipe.machine_id].push_back(recipe.id);
        return true;

    } catch (const std::exception& e) {
        spdlog::warn("Error parsing legacy recipe: {}", e.what());
        return false;
    }
}

void RecipeManager::parseLegacyItemStack(const json& data, std::vector<InputItem>& items) {
    if (!data.is_object()) return;

    uint16_t item_id = 0;
    if (data.contains("item")) {
        if (data["item"].is_number()) {
            item_id = data["item"].get<uint16_t>();
        } else if (data["item"].is_string()) {
            ItemRegistry::instance().loadFromCSV("data/registry/items.csv");
            item_id = ItemRegistry::instance().nameToId(data["item"].get<std::string>());
            if (item_id == 0) { item_id = 1; }
        } else {
            return;
        }
    } else {
        return;
    }

    uint8_t count    = data.value("count", data.value("size", 1U));
    uint16_t meta    = data.value("data", data.value("metadata", 0U));
    bool consume     = data.value("consume", true);

    if (item_id != 0) {
        items.push_back({item_id, count, meta, consume});
    }
}

void RecipeManager::parseLegacyItemStackArray(const json& data, std::vector<InputItem>& items) {
    if (data.is_array()) {
        for (const auto& element : data) {
            parseLegacyItemStack(element, items);
        }
    }
}

uint16_t RecipeManager::stringToMachineId(const std::string& str) {
    if (str == "furnace")           return 36;
    if (str == "macerator")         return 48;
    if (str == "compressor")        return 52;
    if (str == "alloy_smelter")     return 60;
    if (str == "extractor")         return 61;
    if (str == "mixer")             return 62;
    if (str == "assembler")         return 0;
    if (str == "crystallizer")      return 0;
    if (str == "electrolyser" ||
        str == "electrolyzer")      return 0;
    if (str == "chemical_reactor")  return 0;
    return 0;
}

// ---------------------------------------------------------------------------
// RPC implementations
// ---------------------------------------------------------------------------

std::string RecipeManager::checkRecipe(const Protocol::Container* container, uint16_t machine_id) {
    auto containerItems = convertContainerItems(container);

    auto it = recipesByMachineId_.find(machine_id);
    if (it == recipesByMachineId_.end()) return "";

    for (const auto& recipeId : it->second) {
        const auto& recipe = recipes_.at(recipeId);
        if (recipe.matches(containerItems)) {
            return recipe.id;
        }
    }

    return "";
}

const Recipe* RecipeManager::getRecipeById(const std::string& id) const {
    auto it = recipes_.find(id);
    if (it != recipes_.end()) {
        return &it->second;
    }
    return nullptr;
}

const Recipe* RecipeManager::findRecipeByInputs(uint16_t machine_id, const std::vector<ItemStack>& inputs) const {
    auto it = recipesByMachineId_.find(machine_id);
    if (it == recipesByMachineId_.end()) {
        return nullptr;
    }

    // Find the BEST match: among all matching recipes, prefer the one
    // with the most non-empty inputs (most specific = best fit).
    // Without this, a 4-cobble recipe matches before an 8-cobble recipe.
    const Recipe* best = nullptr;
    size_t bestInputs = 0;

    for (const auto& recipeId : it->second) {
        const auto& recipe = recipes_.at(recipeId);
        if (recipe.matches(inputs)) {
            size_t n = 0;
            for (const auto& req : recipe.inputs) {
                if (req.item_id != 0) ++n;
            }
            if (n > bestInputs) {
                bestInputs = n;
                best = &recipe;
            }
        }
    }

    return best;
}

std::unique_ptr<Protocol::Container> RecipeManager::craft(const std::string& recipeId, const Protocol::Container* container) {
    auto recipeIt = recipes_.find(recipeId);
    if (recipeIt == recipes_.end()) {
        spdlog::warn("Recipe not found: {}", recipeId);
        return nullptr;
    }

    const auto& recipe = recipeIt->second;
    auto containerItems = convertContainerItems(container);

    if (!recipe.matches(containerItems)) {
        spdlog::warn("Insufficient inputs for recipe: {}", recipeId);
        return nullptr;
    }

    std::vector<ItemStack> resultItems = recipe.craft(containerItems);
    return createContainer(resultItems);
}

bool RecipeManager::evaluateConditions(const std::string& recipeId) {
    return evaluateConditions(recipeId, MachineState{});
}

bool RecipeManager::evaluateConditions(const std::string& recipeId, const MachineState& state) const {
    auto it = recipes_.find(recipeId);
    if (it == recipes_.end()) return false;
    ConditionEvaluator evaluator;
    return evaluator.evaluate(it->second, state);
}

// ---------------------------------------------------------------------------
// FlatBuffers message handlers
// ---------------------------------------------------------------------------

std::vector<uint8_t> RecipeManager::handleCheckRecipeRequest(const Protocol::CheckRecipeReq* request, uint32_t req_id) {
           std::string recipeId = checkRecipe(request->container(), request->machine_id());

    flatbuffers::FlatBufferBuilder builder;
    auto idOffset = builder.CreateString(recipeId);
    auto respOffset = Protocol::CreateCheckRecipeResp(builder, idOffset);

    Protocol::RecipeReplyBuilder replyBuilder(builder);
    replyBuilder.add_req_id(req_id);
    replyBuilder.add_response_type(Protocol::RecipeResponse_CheckRecipeResp);
    replyBuilder.add_response(respOffset.Union());
    auto replyOffset = replyBuilder.Finish();

    Protocol::RecipeFrameBuilder frameBuilder(builder);
    frameBuilder.add_payload_type(Protocol::RecipePayload_RecipeReply);
    frameBuilder.add_payload(replyOffset.Union());
    auto frameOffset = frameBuilder.Finish();

    builder.Finish(frameOffset);
    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

std::vector<uint8_t> RecipeManager::handleCraftRequest(const Protocol::CraftReq* request, uint32_t req_id) {
    auto newContainer = craft(request->recipe_id()->str(), request->container());
    bool success = (newContainer != nullptr);

    flatbuffers::FlatBufferBuilder builder;

    if (!success) {
        auto msgOffset = builder.CreateString("Craft failed: insufficient inputs or invalid recipe");
        auto errOffset = Protocol::CreateErrorResp(builder, msgOffset);

        Protocol::RecipeReplyBuilder replyBuilder(builder);
        replyBuilder.add_req_id(req_id);
        replyBuilder.add_response_type(Protocol::RecipeResponse_ErrorResp);
        replyBuilder.add_response(errOffset.Union());
        auto replyOffset = replyBuilder.Finish();

        Protocol::RecipeFrameBuilder frameBuilder(builder);
        frameBuilder.add_payload_type(Protocol::RecipePayload_RecipeReply);
        frameBuilder.add_payload(replyOffset.Union());
        auto frameOffset = frameBuilder.Finish();

        builder.Finish(frameOffset);
        return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
    }

    auto respOffset = Protocol::CreateCraftResp(builder, newContainer.get());

    Protocol::RecipeReplyBuilder replyBuilder(builder);
    replyBuilder.add_req_id(req_id);
    replyBuilder.add_response_type(Protocol::RecipeResponse_CraftResp);
    replyBuilder.add_response(respOffset.Union());
    auto replyOffset = replyBuilder.Finish();

    Protocol::RecipeFrameBuilder frameBuilder(builder);
    frameBuilder.add_payload_type(Protocol::RecipePayload_RecipeReply);
    frameBuilder.add_payload(replyOffset.Union());
    auto frameOffset = frameBuilder.Finish();

    builder.Finish(frameOffset);
    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

std::vector<uint8_t> RecipeManager::handleEvaluateConditionsRequest(const Protocol::EvaluateConditionsReq* request, uint32_t req_id) {
    bool ok = evaluateConditions(request->recipe_id()->str());

    flatbuffers::FlatBufferBuilder builder;
    auto respOffset = Protocol::CreateEvaluateConditionsResp(builder, ok);

    Protocol::RecipeReplyBuilder replyBuilder(builder);
    replyBuilder.add_req_id(req_id);
    replyBuilder.add_response_type(Protocol::RecipeResponse_EvaluateConditionsResp);
    replyBuilder.add_response(respOffset.Union());
    auto replyOffset = replyBuilder.Finish();

    Protocol::RecipeFrameBuilder frameBuilder(builder);
    frameBuilder.add_payload_type(Protocol::RecipePayload_RecipeReply);
    frameBuilder.add_payload(replyOffset.Union());
    auto frameOffset = frameBuilder.Finish();

    builder.Finish(frameOffset);
    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

} // namespace RecipeManager
