{
  "title": "MVP: Формат рецепта (MVP Recipe Format)",
  "description": "Define the compact Recipe format. JSON — object keyed by recipe_id. All item IDs = uint16 (numeric). Fields: m (MachineType uint8), in ([[item_id, count?], ...]), out ([[item_id, count?, override?], ...]), dur (ticks), eu (EU/tick). Output supports optional override: display_name, nbt, color, lore. Input supports consume flag.",
  "ecs_components": ["Recipe", "OutputItem"],
  "flatbuffers_schemas": [
    {
      "name": "Recipe",
      "fields": [
        {"name": "id", "type": "string"},
        {"name": "inputs", "type": "[ItemStack]"},
        {"name": "outputs", "type": "[OutputItem]"},
        {"name": "machine", "type": "MachineType"},
        {"name": "duration", "type": "uint32"},
        {"name": "energy_cost", "type": "float"}
      ]
    },
    {
      "name": "OutputItem",
      "fields": [
        {"name": "item_id", "type": "uint16"},
        {"name": "count", "type": "uint8"},
        {"name": "meta", "type": "uint16"},
        {"name": "display_name", "type": "string", "optional": true},
        {"name": "nbt", "type": "[uint8]", "optional": true},
        {"name": "color", "type": "string", "optional": true},
        {"name": "lore", "type": "[string]", "optional": true},
        {"name": "unlocalized_name", "type": "string", "optional": true}
      ]
    }
  ],
  "service_architecture": "Recipe data structure used across all services. Stored in compact JSON (data/recipes/*.json), parsed by RecipeManager at startup. Serialized internally in C++ structs (std::unordered_map<std::string, Recipe>). Item IDs on wire are always uint16 — display names resolved via ItemRegistry.",
  "inputs": {
    "RecipeDefinition": {
      "inputs": ["compact JSON with uint16 item IDs"],
      "outputs": ["parsed Recipe with output overrides"]
    }
  },
  "constraints": [
    "JSON format: {\"recipe_id\": {\"m\": N, \"in\": [[id, cnt?], ...], \"out\": [[id, cnt?, {...}]], \"dur\": N, \"eu\": N}}",
    "Recipe id must be unique globally (not just per file)",
    "in/out use uint16 item IDs — no string names in recipe JSON",
    "count default = 1 if omitted",
    "output must have at least one item for a valid recipe",
    "output override fields: display_name (string), nbt (object: arbitrary JSON → flat NBT), color (HEX), lore (string[]), unlocalized_name (string)",
    "input consume flag: \"consume\": false for containers (buckets, etc.), default = true",
    "MachineType = uint8 enum (0=NONE, 1=FURNACE, 2=ASSEMBLER, 3=CRYSTALLIZER, 4=ELECTROLYSER, 5=CHEMICAL_REACTOR)",
    "eu = 0 means no energy required",
    "No condition fields in MVP (temperature, liquids, etc. are Layer 2)"
  ],
  "test_requirements": "Verify Recipe struct serialization/deserialization via FlatBuffers. Verify compact JSON parsing produces correct internal representation. Verify output override fields parsed and stored. Verify consume=false inputs handled correctly in craft."
}
