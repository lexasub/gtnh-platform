{
  "title": "Layer 2: Категории рецептов (Recipe Categories)",
  "description": "Define recipe categories mapped to machine types (uint8 enum): smelting (FURNACE=1 — temperature, energy), chemical_reactor (CHEMICAL_REACTOR=5 — liquids, purity, energy), machining (ASSEMBLER=2 — energy, duration), assembly_line (energy, liquid components). Each category defines which Layer 2 conditions are valid.",
  "ecs_components": [],
  "flatbuffers_schemas": [
    {
      "name": "RecipeCategory",
      "fields": [
        {"name": "category", "type": "string"},
        {"name": "supported_conditions", "type": "[string]"},
        {"name": "machine_types", "type": "[MachineType]"}
      ]
    }
  ],
  "service_architecture": "Category metadata layer in RecipeManager. Each recipe has a category field that determines which machine type (uint8 enum) can execute it and which additional condition fields are evaluated. Categories extendable without breaking changes.",
  "inputs": {},
  "constraints": [
    "Category 'smelting' supports: temperature, energy — machine = FURNACE (1)",
    "Category 'chemical_reactor' supports: liquids, purity, energy — machine = CHEMICAL_REACTOR (5)",
    "Category 'machining' supports: energy, duration — machine = ASSEMBLER (2)",
    "Category 'assembly_line' supports: energy, liquid components — machine = ASSEMBLER (2)",
    "New categories can be added without modifying existing recipe parsing code",
    "Each recipe belongs to exactly one category",
    "Category sets valid MachineType for the recipe"
  ],
  "test_requirements": "Verify each category maps to correct supported conditions. Verify recipes reject unsupported conditions per category. Verify new categories can be registered at runtime."
}
