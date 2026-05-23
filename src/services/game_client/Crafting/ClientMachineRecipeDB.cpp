#include "ClientMachineRecipeDB.h"
#include <fstream>
#include <dirent.h>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace MachineRecipes {

uint16_t MachineTypeFromFilename(const std::string& name) {
    std::string fname = name;
    if (fname.size() >= 5 && fname.substr(fname.size() - 5) == ".json") {
        fname.resize(fname.size() - 5);
    }

    if (fname == "furnace")           return 36;
    if (fname == "crafting_table")    return 14;

    return 0;
}

void extractLinks(std::vector<ItemStack> &io, nlohmann::basic_json<>::value_type &linkVec) {
    if (linkVec.is_array()) {
        for (const auto& item : linkVec) {
            uint16_t item_id = item[0].get<uint16_t>();
            uint8_t count = (item.size() > 1 && item[1].is_number_unsigned())
                                ? item[1].get<uint8_t>() : 1;
            io.push_back({item_id, count, 0});
        }
    }
}


void LoadFromDirectory(const std::string& dirPath) {
    DIR* dir = opendir(dirPath.c_str());
    if (!dir) {
        spdlog::warn("MachineRecipeDB: failed to open directory {}", dirPath);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() < 5 || name.substr(name.size() - 5) != ".json") {
            continue;
        }

        if (MachineTypeFromFilename(name) == 0) {
            spdlog::warn("MachineRecipeDB: unknown machine type for file {}", name);
            continue;
        }

        std::string path = dirPath;
        if (!path.empty() && path.back() != '/') {
            path += '/';
        }
        path += name;

        std::ifstream file(path);
        if (!file.is_open()) {
            spdlog::warn("MachineRecipeDB: failed to open {}", path);
            continue;
        }

        try {
            nlohmann::json j;
            file >> j;
            file.close();

            for (auto& [key, value] : j.items()) {
                MachineRecipe recipe;
                recipe.name = key;

                extractLinks(recipe.inputs, value["in"]);
                extractLinks(recipe.outputs, value["out"]);

                recipe.duration = value.value("dur", 0u);

                s_recipes[MachineTypeFromFilename(name)].push_back(std::move(recipe));
            }

            spdlog::info("MachineRecipeDB: loaded {} recipes from {}", j.size(), name);
        } catch (const std::exception& e) {
            spdlog::warn("MachineRecipeDB: failed to parse {}: {}", path, e.what());
        }
    }
    closedir(dir);
}

void LoadAll() {
    LoadFromDirectory("data/recipes");
}

}  // namespace MachineRecipes
