{
  "title": "RPC-интерфейс: CheckRecipe и Craft (RPC Interface)",
  "description": "Implement two RPC methods: CheckRecipe(container, machine_type) → recipe_id (string, empty if no match). Craft(recipe_id, container) → new_container (Container, or ErrorResp). Item IDs are uint16 on wire. Input items with consume=false are NOT removed from container on craft. Output items carry overrides (nbt, display_name) in OutputItem.",
  "ecs_components": ["Recipe", "OutputItem", "Container"],
  "flatbuffers_schemas": [
    {
      "name": "CheckRecipeReq",
      "fields": [
        {"name": "container", "type": "Container"},
        {"name": "machine_type", "type": "MachineType"}
      ]
    },
    {
      "name": "CheckRecipeResp",
      "fields": [
        {"name": "recipe_id", "type": "string"}
      ]
    },
    {
      "name": "CraftReq",
      "fields": [
        {"name": "recipe_id", "type": "string"},
        {"name": "container", "type": "Container"}
      ]
    },
    {
      "name": "CraftResp",
      "fields": [
        {"name": "new_container", "type": "Container"}
      ]
    },
    {
      "name": "ErrorResp",
      "fields": [
        {"name": "message", "type": "string"}
      ]
    }
  ],
  "service_architecture": "RPC server using FlatBuffers over TCP through MessageRouter. CheckRecipe matches container contents against recipe inputs for given machine_type. Craft validates recipe_id exists, consumes non-consume=false inputs, adds outputs (with overrides), returns updated container or ErrorResp.",
  "inputs": {
    "CheckRecipe": {
      "inputs": ["container: Container", "machine_type: MachineType"],
      "outputs": ["recipe_id: string"]
    },
    "Craft": {
      "inputs": ["recipe_id: string", "container: Container"],
      "outputs": ["new_container: Container"]
    }
  },
  "constraints": [
    "CheckRecipe matches ALL input ItemStacks (exact item_id uint16, sufficient count, matching metadata)",
    "CheckRecipe returns empty string if no recipe matches or insufficient items",
    "Craft must verify container has all required items before consuming",
    "Craft is atomic: either fully succeeds or fully fails (no partial state)",
    "Input items with consume=false stay in container (bucket used as catalyst, returned empty)",
    "Output items carry override: display_name (string), nbt (byte array), color (string), lore ([string])",
    "Recipe lookup is O(1) via recipe_id map",
    "Item ids are uint16 on wire — zero-copy FlatBuffers ItemStack"
  ],
  "test_requirements": "Verify CheckRecipe matches recipes by uint16 item_id. Verify empty string returned for non-matching containers. Verify Craft consumes inputs (except consume=false) and produces outputs with overrides. Verify Craft fails atomically on insufficient items. Verify edge cases: empty inputs, multiple outputs, stack size, consume=false."
}
