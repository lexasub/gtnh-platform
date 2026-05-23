{
  "title": "Блок верстака и крафт 3×3",
  "description": "Player places block in crafting table, PC client opens craft GUI (ImGui). 3×3 grid with result slot. On \'Craft\' request sent to server.",
  "ecs_components": ["CraftingTable", "RecipeManager", "EntityStateStore", "CRAFTING_TABLE_block_entity"],
  "flatbuffers_schemas": [
    {
      "name": "CraftingTable",
      "fields": [
        {"name": "container_slots", "type": "TileEntity"},
        {"name": "block_id", "type": "uint32"} // block being placed
      ]
    },
    {
      "name": "RecipeManagerRequest",
      "fields": [
        {"name": "player_id", "type": "uint64"},
        {"name": "x", "type": "uint32"},
        {"name": "y", "type": "uint32"},
        {"name": "z", "type": "uint32"},
        {"name": "slots", "type": "array<ItemStack"}
      ]
    },
    {
      "name": "RecipeManagerResponse",
      "fields": [
        {"name": "success", "type": "bool"},
        {"name": "updated_slots", "type": "array<ItemStack"}
      ]
    }
  ],
  "service_architecture": "Client sends Place/Break request to SimulationCore, which validates against crafting recipe and returns InventoryUpdate.",
  "inputs": {
    "PlaceBreak": {
      "inputs": ["player_id", "x", "y", "z", "slots"],
      "outputs": ["player_id", "updated_slots"]
    }
  },
  "constraints": [],
  "test_requirements": "Test block placement in crafting table, verify inventory updates, and ensure recipe lookup works correctly."
}