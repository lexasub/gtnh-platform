# TASK: RecipeManager Service (MVP)
**Layer**: 0
**Status**: Draft
**Epic**: 0-foundation-recipe-system

## Affected Services

| Service | Role |
|---------|------|
| **RecipeManager** ⬅️ NEW | Primary — in-memory recipe storage, JSON loading, lifecycle |
| **MessageRouter** | Transport — dispatches RPCs to RecipeManager |

## Overview

The RecipeManager service provides in-memory recipe storage and lookup for the GTNH Platform. It loads JSON recipe definitions at startup and exposes RPC endpoints for validating containers against recipes and performing crafting operations. This is a Layer 0 service with no conditional logic or external dependencies.

## Service Lifecycle

### Startup

On startup, RecipeManager scans the `data/recipes/*.json` directory and loads all recipe files into memory. Each file contains recipes for a specific machine type (e.g., `furnace`, `blast_furnace`, `fluid_crafting`). Recipes are stored in a map keyed by machine type, then further organized by recipe ID. No recipes are persisted to disk beyond the initial JSON files.

```cpp
// Pseudo-code
void startup() {
    for (auto& file : std::filesystem::directory_iterator("data/recipes")) {
        auto json = std::filesystem::read_text(file.path());
        auto recipes = nlohmann::json::parse(json);
        auto machineType = extractMachineType(file.stem());
        recipesByType[machineType] = recipes;
    }
}
```

### Serve RPCs

After loading recipes, the service listens on the message router for incoming RPC calls. Two operations are supported:

1. **CheckRecipe(container, machine_type)**: Validates whether a given container (item stack or fluid stack) can be used as input for any recipe under the specified machine type. Returns the recipe ID if valid, empty string otherwise.

2. **Craft(recipe_id, container)**: Executes a single crafting step. The container is consumed, and the output is returned. If the container cannot be used by the recipe, an error is returned.

### Shutdown

On shutdown, the service drains pending requests and releases all recipe data. Since the service holds no pointers to external resources, cleanup is trivial.

```cpp
// Pseudo-code
void shutdown() {
    stopListening();
    recipesByType.clear();
}
```

## JSON Parsing

Recipe files use nlohmann/json for parsing. Each file follows a consistent schema:

```json
{
  "machine_type": "furnace",
  "recipes": [
    {
      "id": "coal_coke",
      "input_container": {
        "item": "coal",
        "amount": 1,
        "fluid": null
      },
      "output_container": {
        "item": "coal_coke",
        "amount": 1,
        "fluid": null
      }
    }
  ]
}
```

Schema fields:

| Field | Type | Description |
|-------|------|-------------|
| `machine_type` | string | The machine that uses this recipe set |
| `recipes` | array | List of individual recipes |
| `id` | string | Unique identifier for the recipe |
| `input_container` | object | Container specification |
| `input_container.item` | string | Item ID (empty if fluid) |
| `input_container.amount` | int | Quantity required |
| `input_container.fluid` | string | Fluid ID (empty if item) |
| `output_container` | object | Container specification |
| `output_container.item` | string | Item ID (empty if fluid) |
| `output_container.amount` | int | Quantity produced |
| `output_container.fluid` | string | Fluid ID (empty if item) |

## In-Memory Data Structures

```cpp
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>

namespace recipe {

struct Container {
    std::string item;      // Item ID, empty string if fluid
    int amount;            // Quantity
    std::string fluid;     // Fluid ID, empty string if item
};

struct Recipe {
    std::string id;
    Container input;
    Container output;
};

using RecipeList = std::vector<Recipe>;
using RecipesByType = std::unordered_map<std::string, RecipeList>;

class RecipeManager {
public:
    RecipesByType recipesByType;

    void load(const std::filesystem::path& baseDir);

    std::string checkRecipe(const Container& input, const std::string& machineType) const;
    Container craft(const std::string& recipeId, const Container& input);
};

} // namespace recipe
```

## RPC Dispatch

The message router forwards RPCs to RecipeManager via the following interface:

```cpp
class IRecipeManager {
public:
    virtual ~IRecipeManager() = default;

    virtual std::string checkRecipe(const recipe::Container& input,
                                   const std::string& machineType) = 0;

    virtual recipe::Container craft(const std::string& recipeId,
                                   const recipe::Container& input) = 0;
};
```

The message router uses topic-based routing. RecipeManager subscribes to its own topic and processes incoming calls synchronously.

## File Locations

| Resource | Path | Format |
|----------|------|--------|
| Recipe files | `data/recipes/*.json` | JSON |
| Source files | `src/services/recipe_manager/` | C++ |
| Build output | `build/services/recipe_managerd` | Executable |

## Acceptance Criteria

#### Scenario: Startup loads all recipes

When RecipeManager starts and scans `data/recipes/*.json`, it loads every recipe file and stores recipes by machine type.

Given a `data/recipes/` directory containing:
- `furnace.json` with 5 recipes
- `blast_furnace.json` with 3 recipes

When RecipeManager starts, then:
- All 8 recipes are loaded into memory
- Recipes are organized by `machine_type`
- No recipes are lost or duplicated

#### Scenario: CheckRecipe returns valid recipe ID

When a container matches a recipe input, CheckRecipe returns the recipe ID.

Given RecipeManager has loaded furnace recipes including:
- `coal_coke`: input (item=coal, amount=1)

When CheckRecipe is called with:
- input: (item=coal, amount=1)
- machine_type: furnace

Then the result is `coal_coke`.

#### Scenario: CheckRecipe returns empty string for invalid input

When a container does not match any recipe input, CheckRecipe returns an empty string.

Given RecipeManager has loaded furnace recipes including:
- `coal_coke`: input (item=coal, amount=1)

When CheckRecipe is called with:
- input: (item=iron_ore, amount=1)
- machine_type: furnace

Then the result is an empty string.

#### Scenario: Craft returns the output container

When Craft is called with a valid recipe ID and matching input, it returns the output.

Given RecipeManager has loaded furnace recipes including:
- `coal_coke`: input (item=coal, amount=1) -> output (item=coal_coke, amount=1)

When Craft is called with:
- recipeId: coal_coke
- input: (item=coal, amount=1)

Then the result is (item=coal_coke, amount=1).

#### Scenario: Craft returns error for invalid input

When Craft is called with an invalid container, it returns an error.

Given RecipeManager has loaded furnace recipes including:
- `coal_coke`: input (item=coal, amount=1) -> output (item=coal_coke, amount=1)

When Craft is called with:
- recipeId: coal_coke
- input: (item=iron_ore, amount=1)

Then the result is an error message indicating the input does not match the recipe.
