{
  "title": "Итог: поля рецепта (Recipe Field Summary)",
  "description": "Summary of the compact recipe fields: m (MachineType uint8), in ([[item_id, count?], ...]), out ([[item_id, count?, override?], ...]), dur (ticks), eu (EU/tick). Output override fields: display_name, nbt, color, lore. Input consume flag for containers.",
  "ecs_components": ["Recipe", "OutputItem"],
  "flatbuffers_schemas": [
    {
      "name": "RecipeSummary",
      "fields": [
        {"name": "machine_type", "type": "uint8"},
        {"name": "inputs", "type": "[ItemStack]"},
        {"name": "outputs", "type": "[OutputItem]"},
        {"name": "duration", "type": "uint32"},
        {"name": "energy_cost", "type": "float"}
      ]
    }
  ],
  "service_architecture": "This is the canonical field layout for all MVP recipes. Any recipe loader or parser must accept compact JSON and convert to this internal representation.",
  "inputs": {
    "RecipeData": {
      "inputs": ["m: uint8", "in: [[uint16, count?]]", "out: [[uint16, count?, override?]]", "dur: uint32", "eu: float"],
      "outputs": ["validated Recipe record with OutputItem overrides"]
    }
  },
  "constraints": [
    "input list (in) may be empty (recipes that generate ex nihilo)",
    "output list (out) must have at least one item for a valid recipe",
    "output items MAY have override object: {\"display_name\": str, \"nbt\": obj, \"color\": str, \"lore\": [str]}",
    "input items MAY have {\"consume\": false} to keep container intact after craft",
    "eu = 0 means free (no energy required)",
    "duration must be >= 1 tick",
    "machine_type must be a valid MachineType enum value (uint8 0–5)"
  ],
  "test_requirements": "Verify recipe validation: reject recipes with no outputs, negative duration, invalid machine_type. Verify empty inputs allowed. Verify output override parsed correctly. Verify consume=false input preserved after craft."
}
