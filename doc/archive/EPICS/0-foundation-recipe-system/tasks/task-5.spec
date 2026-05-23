{
  "title": "Service: RecipeManager — загрузка и хранение (Loading & Storage)",
  "description": "Implement RecipeManager as a C++ service that loads all recipes from compact JSON files in data/recipes/ at startup and stores them in memory. Each JSON file is an object keyed by recipe ID (e.g., {\"furnace_beef\": {\"m\":1, \"in\":[[78]], \"out\":[[128]]}}). Item IDs are uint16 — validated at parse time. Output overrides parsed into OutputItem struct.",
  "ecs_components": ["Recipe", "OutputItem", "Container"],
  "flatbuffers_schemas": [
    {
      "name": "LoadRecipesRequest",
      "fields": [
        {"name": "directory", "type": "string"}
      ]
    },
    {
      "name": "LoadRecipesResponse",
      "fields": [
        {"name": "count", "type": "uint32"},
        {"name": "success", "type": "bool"}
      ]
    }
  ],
  "service_architecture": "RecipeManager starts, reads all .json files from data/recipes/, parses compact JSON using nlohmann/json, validates uint16 range for item IDs, converts to internal C++ structs (Recipe with OutputItem overrides), and stores in std::unordered_map<std::string, Recipe>. Secondary index recipesByMachineType_ for O(1) per-machine lookups. GC not required.",
  "inputs": {
    "LoadRecipes": {
      "inputs": ["directory: string"],
      "outputs": ["count: uint32", "success: bool"]
    }
  },
  "constraints": [
    "JSON format: object keyed by recipe_id, each value = recipe object (not array)",
    "item_id must be uint16 (0–65535) — reject and log if out of range",
    "JSON parsing uses nlohmann/json for MVP speed",
    "All recipes loaded at startup; no hot-reload in MVP",
    "Recipes stored in memory until service shutdown",
    "Each data/recipes/*.json file contains one machine type or category",
    "Recipe IDs must be unique across all files (collision = last-write-wins with warning)",
    "Output overrides parsed into OutputItem struct (display_name, nbt, color, lore)",
    "Input consume flag parsed into ItemStack extension (consume: bool, default=true)",
    "Must handle malformed JSON gracefully (log error, skip file, continue startup)"
  ],
  "test_requirements": "Verify loading from valid compact JSON produces correct count of Recipe objects with correct OutputItem overrides. Verify uint16 overflow rejected. Verify empty directory handled. Verify duplicate recipe_id warning."
}
