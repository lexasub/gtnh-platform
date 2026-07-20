#include "RecipeManager.h"
#include "ItemRegistry.h"
#include "ConditionEvaluator.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <climits>

namespace RecipeManager {

// ---------------------------------------------------------------------------
// Recipe matching & crafting
// ---------------------------------------------------------------------------

bool Recipe::matches(const std::vector<ItemStack>& container_items) const {
    size_t requiredInputs = 0;
    for (const auto& req : inputs) {
        if (req.item_id != 0) ++requiredInputs;
    }
    size_t available = 0;
    for (const auto& slot : container_items) {
        if (slot.item_id != 0 && slot.count > 0) ++available;
    }
    if (available < requiredInputs) return false;

    std::vector<bool> used(container_items.size(), false);

    for (const auto& req : inputs) {
        if (req.item_id == 0) continue;
        bool found = false;
        for (size_t i = 0; i < container_items.size(); ++i) {
            if (used[i]) continue;
            const auto& slot = container_items[i];
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
    std::vector<ItemStack> result = container_items;

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

    for (const auto& out : outputs) {
        bool stacked = false;
        for (auto& slot : result) {
            if (slot.item_id == out.item_id && slot.metadata == out.metadata) {
                uint8_t space = 64 - slot.count;
                if (space >= out.count) {
                    slot.count += out.count;
                    stacked = true;
                    break;
                }
            }
        }
        if (stacked) continue;

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

// ===========================================================================
// YAML: Machine registry loading
// ===========================================================================

EnergyType RecipeManager::parseEnergyType(const std::string& str) const {
    if (str.empty()) return static_cast<EnergyType>(255);
    if (str == "HEAT") return EnergyType::HEAT;
    if (str == "STEAM") return EnergyType::STEAM;
    return EnergyType::ELECTRICITY;
}

bool RecipeManager::loadMachinesFromYaml(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        spdlog::warn("Failed to open machines YAML: {}", filePath);
        return false;
    }

    YAML::Node root;
    try {
        root = YAML::Load(file);
    } catch (const YAML::Exception& e) {
        spdlog::warn("Failed to parse machines YAML {}: {}", filePath, e.what());
        return false;
    }

    if (!root["machine_classes"]) {
        spdlog::warn("machines.yaml: no 'machine_classes' section");
        return false;
    }

    bool result = parseYamlMachines(root);
    spdlog::info("Loaded {} machine classes, {} block_id mappings",
                 classes_.size(), classByBlockId_.size());
    return result;
}

bool RecipeManager::parseYamlMachines(const YAML::Node& root) {
    auto classes = root["machine_classes"];
    for (size_t i = 0; i < classes.size(); ++i) {
        if (!parseYamlMachineClass(classes[i])) {
            spdlog::warn("machines.yaml: failed to parse class at index {}", i);
        }
    }
    return true;
}

bool RecipeManager::parseYamlMachineClass(const YAML::Node& node) {
    if (!node["class"] || !node["variants"]) {
        return false;
    }

    MachineClassDef def;
    def.name = node["class"].as<std::string>();

    auto variants = node["variants"];
    for (size_t i = 0; i < variants.size(); ++i) {
        const auto& v = variants[i];
        MachineVariant mv;
        mv.block_id       = v["block_id"].as<uint16_t>(0);
        mv.name           = v["name"].as<std::string>("");
        mv.energy_in      = parseEnergyType(v["energy_in"].as<std::string>(""));
        mv.energy_out     = parseEnergyType(v["energy_out"].as<std::string>(""));
        mv.tier           = v["tier"].as<int16_t>(0);

        def.variants.push_back(mv);

        if (mv.block_id > 0) {
            if (classByBlockId_.count(mv.block_id) > 0) {
                spdlog::warn("Block ID {} already mapped to class '{}', overriding with '{}'",
                             mv.block_id, classByBlockId_[mv.block_id], def.name);
            }
            classByBlockId_[mv.block_id] = def.name;
            tierByBlockId_[mv.block_id] = mv.tier;
            energyInByBlockId_[mv.block_id] = static_cast<uint8_t>(mv.energy_in);
        }
    }

    classes_[def.name] = std::move(def);
    return true;
}

int16_t RecipeManager::getMachineTier(uint16_t block_id) const {
    auto it = tierByBlockId_.find(block_id);
    return (it != tierByBlockId_.end()) ? it->second : 0;
}

uint8_t RecipeManager::getMachineEnergyIn(uint16_t block_id) const {
    auto it = energyInByBlockId_.find(block_id);
    return (it != energyInByBlockId_.end()) ? it->second : ENERGY_TYPE_ANY;
}

const std::string& RecipeManager::getMachineClass(uint16_t block_id) const {
    auto it = classByBlockId_.find(block_id);
    return (it != classByBlockId_.end()) ? it->second : emptyString_;
}

// ===========================================================================
// YAML: Recipe loading
// ===========================================================================

bool RecipeManager::loadRecipesFromYamlFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        spdlog::warn("Failed to open YAML recipe file: {}", filePath);
        return false;
    }

    YAML::Node root;
    try {
        root = YAML::Load(file);
    } catch (const YAML::Exception& e) {
        spdlog::warn("Failed to parse YAML recipe {}: {}", filePath, e.what());
        return false;
    }

    // Optional top-level class override
    std::string defaultClass;
    if (root["class"]) {
        defaultClass = root["class"].as<std::string>();
    }

    if (!root["recipes"]) {
        spdlog::warn("YAML recipe file {}: no 'recipes' section", filePath);
        return false;
    }

    return parseYamlRecipes(root["recipes"], defaultClass);
}

bool RecipeManager::loadRecipesFromYamlDirectory(const std::string& directoryPath) {
    spdlog::info("Loading YAML recipes from directory: {}", directoryPath);

    try {
        std::vector<std::string> recipeFiles;
        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            auto ext = entry.path().extension();
            if (entry.is_regular_file() && (ext == ".yaml" || ext == ".yml")) {
                recipeFiles.push_back(entry.path().string());
            }
        }

        if (recipeFiles.empty()) {
            spdlog::warn("No YAML recipe files found in directory: {}", directoryPath);
            return true;
        }

        for (const auto& filePath : recipeFiles) {
            if (loadRecipesFromYamlFile(filePath)) {
                // Count recipes loaded from this file
                // (We count them by scanning recipes_ — but since they're
                // already added there, just note the file was loaded)
                spdlog::info("Loaded YAML recipes from {}", filePath);
            }
        }

        // Count recipes with machine_class set (YAML-originated)
        size_t yamlCount = 0;
        for (const auto& [id, recipe] : recipes_) {
            if (!recipe.machine_class.empty()) ++yamlCount;
        }
        spdlog::info("Loaded {} total YAML-sourced recipes", yamlCount);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("Error loading YAML recipes from directory {}: {}", directoryPath, e.what());
        return false;
    }
}

bool RecipeManager::parseYamlRecipes(const YAML::Node& root, const std::string& defaultClass) {
    if (!root.IsSequence()) {
        spdlog::warn("YAML 'recipes' is not a sequence");
        return false;
    }

    size_t loaded = 0;
    for (size_t i = 0; i < root.size(); ++i) {
        if (parseYamlRecipe(root[i], defaultClass)) {
            ++loaded;
        }
    }
    return loaded > 0;
}

uint16_t RecipeManager::resolveItemName(const std::string& name) const {
    return ItemRegistry::instance().nameToId(name);
}

bool RecipeManager::parseYamlRecipe(const YAML::Node& yaml, const std::string& defaultClass) {
    try {
        Recipe recipe;

        // Identifier
        if (!yaml["name"]) {
            spdlog::warn("YAML recipe missing 'name' field");
            return false;
        }
        recipe.id = yaml["name"].as<std::string>();

        // Class (per-recipe override or default)
        if (yaml["class"]) {
            recipe.machine_class = yaml["class"].as<std::string>();
        } else if (!defaultClass.empty()) {
            recipe.machine_class = defaultClass;
        } else {
            spdlog::warn("YAML recipe '{}' has no machine class", recipe.id);
            return false;
        }

        // Tier bounds (inclusive)
        recipe.min_tier = yaml["min_tier"].as<int16_t>(0);
        recipe.max_tier = yaml["max_tier"].as<int16_t>(32767);

        // Energy type filter (optional — ENERGY_TYPE_ANY = matches all)
        if (yaml["energy_in"]) {
            std::string ei = yaml["energy_in"].as<std::string>();
            if (ei == "HEAT")       recipe.energy_type = static_cast<uint8_t>(1);
            else if (ei == "STEAM") recipe.energy_type = static_cast<uint8_t>(2);
            else if (ei == "ELECTRICITY") recipe.energy_type = static_cast<uint8_t>(0);
            else if (ei == "ROTATION") recipe.energy_type = static_cast<uint8_t>(3);
            else spdlog::warn("YAML recipe '{}': unknown energy_in '{}'", recipe.id, ei);
        }

        // Inputs
        if (yaml["inputs"]) {
            auto inputs = yaml["inputs"];
            if (inputs.IsSequence()) {
                for (size_t i = 0; i < inputs.size(); ++i) {
                    auto item = parseYamlInputItem(inputs[i]);
                    if (item.item_id != 0 || inputs[i]["item"]) {
                        // Even id=0 items can be valid if explicitly set
                        recipe.inputs.push_back(item);
                    }
                }
            }
        }

        // Outputs (required unless generator/boiler — they produce energy_output, not items)
        bool isProducer = (recipe.machine_class == "generator" || recipe.machine_class == "boiler");
        if (!isProducer) {
            if (!yaml["outputs"] || !yaml["outputs"].IsSequence()) {
                spdlog::warn("YAML recipe '{}': missing or invalid 'outputs'", recipe.id);
                return false;
            }
            auto outputs = yaml["outputs"];
            for (size_t i = 0; i < outputs.size(); ++i) {
                recipe.outputs.push_back(parseYamlOutputItem(outputs[i]));
            }
            if (recipe.outputs.empty()) {
                spdlog::warn("YAML recipe '{}': empty outputs", recipe.id);
                return false;
            }
        }

        // Duration
        recipe.duration = yaml["duration"].as<uint32_t>(200);
        if (recipe.duration == 0) recipe.duration = 1;

        // Energy cost (optional)
        recipe.energy_cost = yaml["eu"].as<float>(0.0f);

        // Energy output (optional — >0 for generators/boilers)
        recipe.energy_output = yaml["energy_output"].as<float>(0.0f);

        // Conditions (optional)
        if (yaml["conditions"]) {
            parseYamlConditions(yaml["conditions"], recipe.conditions);
        }

        // Store
        recipes_[recipe.id] = recipe;
        recipesByClass_[recipe.machine_class].push_back(recipe.id);

        return true;

    } catch (const std::exception& e) {
        if (yaml["name"]) {
            spdlog::warn("Error parsing YAML recipe '{}': {}", yaml["name"].as<std::string>(), e.what());
        } else {
            spdlog::warn("Error parsing unnamed YAML recipe: {}", e.what());
        }
        return false;
    }
}

InputItem RecipeManager::parseYamlInputItem(const YAML::Node& node) {
    InputItem item;
    item.item_id = 0;
    item.count = 1;
    item.consume = true;
    item.replace_item = 0;
    item.replace_meta = 0;

    if (!node.IsMap()) return item;

    // Resolve item name or numeric ID
    if (node["item"]) {
        if (node["item"].IsScalar()) {
            std::string itemStr = node["item"].as<std::string>();
            // Try numeric first
            try {
                item.item_id = static_cast<uint16_t>(std::stoi(itemStr));
            } catch (...) {
                // String name — resolve via registry
                item.item_id = resolveItemName(itemStr);
            }
        } else {
            item.item_id = node["item"].as<uint16_t>(0);
        }
    }

    item.count = node["count"].as<uint8_t>(1);

    if (node["consume"]) {
        item.consume = node["consume"].as<bool>(true);
    }

    if (node["replace"]) {
        std::string replaceStr = node["replace"].as<std::string>("");
        if (!replaceStr.empty()) {
            item.replace_item = resolveItemName(replaceStr);
        }
    }
    if (node["replace_meta"]) {
        item.replace_meta = node["replace_meta"].as<uint16_t>(0);
    }

    return item;
}

OutputItem RecipeManager::parseYamlOutputItem(const YAML::Node& node) {
    OutputItem out;
    out.item_id = 0;
    out.count = 1;
    out.metadata = 0;

    if (!node.IsMap()) return out;

    // Resolve item name or numeric ID
    if (node["item"]) {
        if (node["item"].IsScalar()) {
            std::string itemStr = node["item"].as<std::string>();
            try {
                out.item_id = static_cast<uint16_t>(std::stoi(itemStr));
            } catch (...) {
                out.item_id = resolveItemName(itemStr);
            }
        } else {
            out.item_id = node["item"].as<uint16_t>(0);
        }
    }

    out.count = node["count"].as<uint8_t>(1);
    out.metadata = node["meta"].as<uint16_t>(0);

    // Optional display metadata
    if (node["display_name"]) {
        out.display_name = node["display_name"].as<std::string>();
    }
    if (node["color"]) {
        out.color = node["color"].as<std::string>();
    }
    if (node["unlocalized_name"]) {
        out.unlocalized_name = node["unlocalized_name"].as<std::string>();
    }
    if (node["lore"] && node["lore"].IsSequence()) {
        std::vector<std::string> loreVec;
        for (size_t i = 0; i < node["lore"].size(); ++i) {
            loreVec.push_back(node["lore"][i].as<std::string>());
        }
        out.lore = loreVec;
    }

    return out;
}

void RecipeManager::parseYamlConditions(const YAML::Node& node, RecipeConditions& conditions) {
    if (!node.IsMap()) return;

    // Environment temperature
    if (node["temperature"]) {
        if (!conditions.environment) conditions.environment.emplace();
        if (!conditions.environment->temperature) conditions.environment->temperature.emplace();
        auto temp = node["temperature"];
        if (temp["min"]) conditions.environment->temperature->min = temp["min"].as<float>();
        if (temp["max"]) conditions.environment->temperature->max = temp["max"].as<float>();
    }

    // Purity
    if (node["purity"]) {
        if (!conditions.environment) conditions.environment.emplace();
        conditions.environment->purity = node["purity"].as<float>();
    }

    // Biomes
    if (node["biomes"] && node["biomes"].IsSequence()) {
        if (!conditions.environment) conditions.environment.emplace();
        for (size_t i = 0; i < node["biomes"].size(); ++i) {
            if (node["biomes"][i].IsScalar()) {
                try {
                    conditions.environment->biomes.push_back(
                        node["biomes"][i].as<uint16_t>());
                } catch (...) {
                    // String biome names would need a biome registry
                }
            }
        }
    }

    // Machine energy conditions
    if (node["machine"]) {
        auto mach = node["machine"];
        if (!conditions.machine) conditions.machine.emplace();
        if (mach["energy_min"]) conditions.machine->energy_min = mach["energy_min"].as<uint32_t>();
        if (mach["energy_max"]) conditions.machine->energy_max = mach["energy_max"].as<uint32_t>();
        if (mach["network_id"]) conditions.machine->network_id = mach["network_id"].as<uint32_t>();
        if (mach["facing"])     conditions.machine->facing = mach["facing"].as<uint8_t>();
    }
}

// ===========================================================================
// RPC implementations
// ===========================================================================

std::string RecipeManager::checkRecipe(const Protocol::Container* container, uint16_t machine_id) {
    auto containerItems = convertContainerItems(container);

    auto classIt = classByBlockId_.find(machine_id);
    if (classIt != classByBlockId_.end()) {
        const std::string& machineClass = classIt->second;
        int16_t machineTier = getMachineTier(machine_id);
        uint8_t machineEnergyIn = getMachineEnergyIn(machine_id);

        auto recipesIt = recipesByClass_.find(machineClass);
        if (recipesIt != recipesByClass_.end()) {
            // Try recipes from most specific (highest min_tier ≤ machine tier)
            // to least specific (lowest min_tier)
            std::vector<const Recipe*> candidates;
            for (const auto& recipeId : recipesIt->second) {
                const auto& recipe = recipes_.at(recipeId);
                if (recipe.min_tier <= machineTier && machineTier <= recipe.max_tier) {
                    if (recipe.energy_type != ENERGY_TYPE_ANY && recipe.energy_type != machineEnergyIn) {
                        continue;
                    }
                    if (recipe.matches(containerItems)) {
                        candidates.push_back(&recipe);
                    }
                }
            }

            // Pick the best: highest min_tier wins (most specific)
            if (!candidates.empty()) {
                const Recipe* best = nullptr;
                int16_t bestMinTier = -1;
                for (const auto* r : candidates) {
                    if (r->min_tier > bestMinTier) {
                        bestMinTier = r->min_tier;
                        best = r;
                    }
                }
                if (best) return best->id;
            }
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
    auto classIt = classByBlockId_.find(machine_id);
    if (classIt != classByBlockId_.end()) {
        const std::string& machineClass = classIt->second;
        int16_t machineTier = getMachineTier(machine_id);
        uint8_t machineEnergyIn = getMachineEnergyIn(machine_id);

        auto recipesIt = recipesByClass_.find(machineClass);
        if (recipesIt != recipesByClass_.end()) {
            const Recipe* best = nullptr;
            int16_t bestMinTier = -1;

            for (const auto& recipeId : recipesIt->second) {
                const auto& recipe = recipes_.at(recipeId);
                if (recipe.min_tier <= machineTier && machineTier <= recipe.max_tier) {
                    if (recipe.energy_type != ENERGY_TYPE_ANY && recipe.energy_type != machineEnergyIn) {
                        continue;
                    }
                    if (recipe.matches(inputs)) {
                        // Prefer higher min_tier (more specific to this tier)
                        if (recipe.min_tier > bestMinTier) {
                            bestMinTier = recipe.min_tier;
                            best = &recipe;
                        }
                    }
                }
            }

            return best;
        }
    }

    return nullptr;
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
