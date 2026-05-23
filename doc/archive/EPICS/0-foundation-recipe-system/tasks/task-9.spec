{
  "title": "Layer 2: Проверка условий — EvaluateConditions (Condition Evaluation RPC)",
  "description": "Add EvaluateConditions RPC method. Takes recipe_id and MachineState, evaluates condition fields against machine state, returns bool. MachineState: temperature, liquid_levels (Liquid[] with uint32 fluid_id), energy, purity, biome, duration. Matches actual recipe.fbs schema: EvaluateConditionsResp { success:bool }.",
  "ecs_components": ["MachineState"],
  "flatbuffers_schemas": [
    {
      "name": "EvaluateConditionsReq",
      "fields": [
        {"name": "recipe_id", "type": "string"},
        {"name": "machine_state", "type": "MachineState"}
      ]
    },
    {
      "name": "EvaluateConditionsResp",
      "fields": [
        {"name": "success", "type": "bool"}
      ]
    }
  ],
  "service_architecture": "RPC on RecipeManager. Called by SimulationCore or Machines to verify all recipe conditions are met before executing craft. MachineState populated by caller (temperature, liquid_levels with uint32 fluid_ids, etc.). Returns bool only (failed conditions list not on wire — debugging via logs).",
  "inputs": {
    "EvaluateConditions": {
      "inputs": ["recipe_id: string", "machine_state: MachineState"],
      "outputs": ["success: bool"]
    }
  },
  "constraints": [
    "EvaluateConditions only checks conditions, does not modify state",
    "Always returns true for recipes with no condition fields",
    "MachineState fields not relevant to the recipe are ignored",
    "Temperature source: provided by Machine service via RPC",
    "Liquid source: provided by Machine service via RPC (tank contents, uint32 fluid_id)",
    "Purity: provided by Machine service via RPC",
    "Biome: provided by Machine service via RPC (from SpatialIndex or SimulationCore)",
    "Fluid IDs are uint32 on wire — zero-copy, string names resolved separately via registry"
  ],
  "test_requirements": "Verify EvaluateConditions returns true when all conditions match MachineState. Verify returns false when any condition fails. Verify no-condition recipes return true. Verify MachineState with missing fields does not error."
}
