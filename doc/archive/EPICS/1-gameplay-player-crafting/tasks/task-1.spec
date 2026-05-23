{
  "title": "Игрок и крафт (Player & Crafting)",
  "description": "Mechanics interaction between player and world: inventory, hotbar, block placement/breaking, item usage, crafting through crafting table, and resource gathering via creative menu.",
  "ecs_components": ["PlayerInventory", "CraftingManager", "RecipeManager"],
  "flatbuffers_schemas": [
    {
      "name": "PlayerAction",
      "fields": [
        {"name": "player_id", "type": "uint64"},
        {"name": "action", "type": "PlayerActionType"},
        {"name": "x", "type": "uint32"},
        {"name": "y", "type": "uint32"},
        {"name": "z", "type": "uint32"},
        {"name": "block_id", "type": "uint16"},
        {"name": "selected_slot", "type": "uint8"}
      ]
    },
    {
      "name": "InventoryUpdate",
      "fields": [
        {"name": "player_id", "type": "uint64"},
        {"name": "slots", "type": "array<ItemStack>"}
      ]
    }
  ],
  "service_architecture": "Client sends CraftRequest to SimulationCore, which queries RecipeManager, checks for recipe, and returns updated inventory. Inventory update is sent back to client.",
  "inputs": {
    "CraftRequest": {
      "inputs": ["player_id", "x", "y", "z", "selected_slot"],
      "outputs": ["player_id", "updated_slots"]
    }
  },
  "constraints": [
    "No falling items",
    "No tools (any item breaks block equally)",
    "No durability (items don\'t wear in MVP)",
    "Full client trust (client reports actions to server)"
  ],
  "test_requirements": "Verify crafting works with specific recipe, inventory updates correctly, and PlayerAction includes selected_slot field. Ensure recipe lookup and block breaking logic works as expected."
}