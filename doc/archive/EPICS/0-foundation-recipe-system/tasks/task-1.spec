{
  "title": "Система рецептов: обзор (Recipe System Overview)",
  "description": "Define the Recipe System architecture: separate RecipeManager + ItemRegistry services. Recipes stored as compact JSON with uint16 item IDs. All services use uint16 on wire (FlatBuffers). ItemRegistry = SQLite read-only, lazy metadata loading for display. For MVP, several JSON recipe files (furnace, assembler, etc.). Must be accessible to SimulationCore via RPC.",
  "ecs_components": ["Container", "MachineState"],
  "flatbuffers_schemas": [
    {
      "name": "Container",
      "fields": [
        {"name": "items", "type": "[ItemStack:9]"},
        {"name": "dirty", "type": "uint8"},
        {"name": "size", "type": "uint16"}
      ]
    },
    {
      "name": "MachineState",
      "fields": [
        {"name": "temperature", "type": "float"},
        {"name": "liquid_levels", "type": "[Liquid]"},
        {"name": "energy", "type": "float"},
        {"name": "purity", "type": "float"},
        {"name": "biome", "type": "string"},
        {"name": "duration", "type": "uint32"}
      ]
    }
  ],
  "service_architecture": "RecipeManager is a separate C++ service library responsible for loading, storing, and providing an RPC interface for recipe checking and execution. ItemRegistry is a SQLite database (data/registry/items.db) shared read-only across all services — provides item_id→name/stack_size mapping, lazy-loaded per-item for display. Accessed by SimulationCore for craft verification and by Machines for recipe execution. Data flows via FlatBuffers over TCP (through MessageRouter).",
  "inputs": {},
  "constraints": [
    "RecipeManager is a separate service library (not embedded in SimulationCore)",
    "ItemRegistry = SQLite read-only, CSV as source of truth under git, SQLite as build artifact",
    "All item IDs are uint16 (0–65535) on wire (FlatBuffers) and in C++ hot-path",
    "MVP uses compact JSON recipe files (data/recipes/*.json, object keyed by recipe_id)",
    "Must be accessible to SimulationCore via RPC",
    "No hot-reload in MVP (all recipes loaded at startup, no persistence)"
  ],
  "test_requirements": "Verify RecipeManager service starts and registers with MessageRouter. Verify that SimulationCore can connect and send requests. Verify that MVP recipes are accessible via CheckRecipe/Craft RPCs. Verify ItemRegistry loads from SQLite and returns correct item_id→name mapping."
}
