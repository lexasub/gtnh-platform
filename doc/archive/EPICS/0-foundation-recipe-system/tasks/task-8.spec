{
  "title": "Layer 2: Дополнительные условия (Additional Conditions)",
  "description": "Extend Recipe JSON with optional Layer 2 fields: temp (min/max °C), liquid ([[fluid_id, amount], ...]), purity (0.0–1.0), biome (string[]), special (string). All optional. Fluid IDs are uint32 (0 = empty). Pure numeric wire format — string names only for display via registry.",
  "ecs_components": [],
  "flatbuffers_schemas": [
    {
      "name": "RecipeCondition",
      "fields": [
        {"name": "temperature_min", "type": "float", "optional": true},
        {"name": "temperature_max", "type": "float", "optional": true},
        {"name": "liquid_components", "type": "[Liquid]", "optional": true},
        {"name": "purity", "type": "float", "optional": true},
        {"name": "biome", "type": "[string]", "optional": true},
        {"name": "special", "type": "string", "optional": true}
      ]
    }
  ],
  "service_architecture": "Optional condition fields stored as part of the Recipe data structure in RecipeManager. When EvaluateConditions is called, fields are compared against MachineState from calling service (SimulationCore). Condition fields absent in recipe = not checked.",
  "inputs": {},
  "constraints": [
    "All condition fields are optional — missing fields are not evaluated",
    "JSON field names: temp ({\"min\": float, \"max\": float}), liquid ([[fluid_id: uint32, amount: int32]]), purity (float), biome ([string]), special (string)",
    "temperature is range (min, max); recipe matches if machine temp is within [min, max] inclusive",
    "liquid_components requires ALL listed fluids to be present in machine (sufficient amount)",
    "purity requires machine purity >= specified value (0.0–1.0)",
    "biome requires machine to be in one of the listed biomes",
    "special is extensible string slot for future use (player proximity, time of day, etc.)",
    "Fluid IDs are uint32 on wire — no string fluid names in recipes"
  ],
  "test_requirements": "Verify conditions correctly parsed from compact JSON. Verify temperature range matching (inclusive bounds). Verify liquid amount sufficiency check. Verify purity threshold check. Verify biome list contains check. Verify all-conditions-met passes. Verify any-condition-failing rejects. Verify missing optional fields do not cause errors."
}
