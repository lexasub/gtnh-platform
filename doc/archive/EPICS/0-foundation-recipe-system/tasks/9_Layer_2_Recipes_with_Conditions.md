### Layer 2: Recipes with Conditions

Extension for RecipeManager to handle recipes with conditions such as temperature, energy, etc.

| Condition | Type | Description |
|-----------|------|-------------|
| `temperature` | number | Machine temperature (in °C). Recipes may require min/max temperature. |
| `energy_cost` | number | Energy in ticks. If 0, energy not considered. |
| `duration` | number | Duration in ticks. |
| `liquid_components` | Liquid[] | Liquid components that must be present in the machine. |
| `purity` | number | Required purity (0–100). |
| `biome` | Biome[] | Required biomes for execution. |
| `special_condition` | string | Slot for custom conditions (e.g., player presence, time of day). |

### FlatBuffers Schema

Reference: `src/protocol/0-foundation-recipe-system/0-foundation-recipe-system.fbs`