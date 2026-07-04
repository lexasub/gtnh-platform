# TASK: Recipe Data Format & Schema
**Layer**: 0
**Status**: Draft
**Epic**: 0-foundation-recipe-system

## Affected Services

| Service | Role |
|---------|------|
| **RecipeManager** ⬅️ NEW | Owns the schema — loads JSON recipes at startup |
| **Protocol** | Shared FlatBuffer types (`ItemStack`, `Container`) |
| **SimulationCore** | Consumer — reads recipe data during machine tick |
| **GameClient** | Consumer — displays recipe info in GUI |

---

## Overview

This specification defines the data format and schema for recipes in the GTNH Platform foundation. Recipes represent the core logic of item crafting, machine processing, and multiblock operations across the system.

The format supports JSON serialization for external data files, while maintaining a clean TypeScript interface for internal service communication.

## Recipe Interface

```typescript
interface Recipe {
    id: string;
    inputs: ItemStack[];
    outputs: ItemStack[];
    machine: MachineType;
    duration: number;
    energyCost: number;
}
```

| Field | Type | Description |
|------|------|-------------|
| `id` | string | Unique identifier for the recipe. Format: `{machine}:{recipe_key}` (e.g., `furnace:iron_ingot_smelt`). Must be globally unique across all machines. |
| `inputs` | `ItemStack[]` | Ordered list of input items. Order matters for machines with positional constraints (e.g., smelting chains). Empty array for machine-less recipes. |
| `outputs` | `ItemStack[]` | Ordered list of output items. Order matters for machines with multiple outputs (e.g., blast furnace with multiple slots). |
| `machine` | `MachineType` | Required machine type, or `0` for machine-less recipes (crafting table, manual crafting). |
| `duration` | number | Processing time in ticks (20 ticks = 1 second). Use `0` for machine-less recipes. |
| `energyCost` | number | Energy consumed per tick in RF. Use `0` for machines that don't consume energy. |

## ItemStack Type

Represents a single item with quantity and optional metadata.

```typescript
interface ItemStack {
    item: string;
    count: number;
    metadata?: number;
}
```

| Field | Type | Description |
|------|------|-------------|
| `item` | string | Item identifier in the format `namespace:item_name`. Use `base:` for vanilla items. |
| `count` | number | Quantity of items, must be positive (1+). |
| `metadata` | number | Item NBT tag or damage value. Use `0` if unspecified. |

**Example:**
```json
{
    "item": "base:iron_ore",
    "count": 1,
    "metadata": 0
}
```

## MachineType Enum

```typescript
enum MachineType {
    WORKBENCH = 0,
    FURNACE = 1,
    BLAST_FURNACE = 2,
    CRAFTING_TABLE = 3,
    // Add more as needed
}
```

| Value | Type | Description |
|-------|------|-------------|
| `0` | WORKBENCH | Crafting table / manual crafting. |
| `1` | FURNACE | Basic smelting machine. |
| `2` | BLAST_FURNACE | Advanced smelting with additional mechanics. |
| `3` | CRAFTING_TABLE | Minecraft crafting table. |
| `4+` | EXTENSIBLE | New machine types extend from here. |

## JSON File Format

Each machine category uses a **single dedicated file** containing all recipes for that machine type.

```json
[
    {
        "id": "base:crafting/stone_pickaxe",
        "inputs": [
            {
                "item": "base:stick",
                "count": 2
            }
        ],
        "outputs": [
            {
                "item": "base:stone_pickaxe",
                "count": 1
            }
        ],
        "machine": 3,
        "duration": 0,
        "energyCost": 0
    },
    {
        "id": "base:furnace/iron_ore_smelt",
        "inputs": [
            {
                "item": "base:iron_ore",
                "count": 1
            }
        ],
        "outputs": [
            {
                "item": "base:iron_ingot",
                "count": 1
            }
        ],
        "machine": 1,
        "duration": 100,
        "energyCost": 0
    }
]
```

### File Naming Convention

Files are stored in `data/recipes/{machine_type}/` and named `recipes.json`:

```
data/recipes/
├── 0-workbench/recipes.json
├── 1-furnace/recipes.json
├── 2-blast-furnace/recipes.json
├── 3-crafting-table/recipes.json
└── 4-unknown/recipes.json
```

The machine type in the path matches the `MachineType` enum value for easy validation and cross-referencing.

## Data Locations

Recipe data files reside in:

```
data/recipes/{machine_type}/recipes.json
```

Where `machine_type` is the string representation of the `MachineType` enum.

### Loading Order

1. Base vanilla recipes (`base:` namespace) load first
2. Mod-added recipes load second
3. Recipe manager validates and deduplicates by `id` field

## Acceptance Criteria

#### Scenario: Vanilla crafting recipe loads correctly
**WHEN** a crafting table recipe is stored as `data/recipes/3-crafting-table/recipes.json`  
**AND** it contains valid `ItemStack` entries with proper `namespace:item` format  
**THEN** the recipe manager successfully parses all entries  
**AND** each entry validates against the `Recipe` interface schema  
**AND** `machine` field equals `3` (CRAFTING_TABLE)  
**AND** `duration` and `energyCost` equal `0`

#### Scenario: Furnace smelting recipe with duration and energy
**WHEN** a furnace recipe is stored as `data/recipes/1-furnace/recipes.json`  
**AND** it includes `duration` of `100` ticks and `energyCost` of `0`  
**THEN** the recipe manager assigns the recipe to `MachineType.FURNACE`  
**AND** the duration value is preserved for processing time calculations  
**AND** the energy cost is recorded as zero

#### Scenario: Machine-less recipe with empty inputs
**WHEN** a machine-less recipe is stored with `machine: 0`  
**AND** `inputs` is an empty array `[]`  
**AND** `outputs` contains exactly one `ItemStack`  
**THEN** the recipe manager treats this as a direct conversion  
**AND** no machine requirement is enforced  
**AND** the recipe is still valid and loadable

## Open Questions

### Data Organization Strategy

**Question:** Should recipes use a single shared file across all machines, or separate files per machine category?

**Proposed Decision:** **Separate files per machine category** (as specified above).

**Rationale:**
- Clear separation of concerns — each machine type has its own data file
- Easier to validate and debug — missing file means missing machine type
- Simpler to extend — adding a new machine type only requires creating a new directory
- Performance — smaller files load faster and can be lazy-loaded per machine type
- File organization matches the `MachineType` enum, making cross-referencing straightforward

Alternative considered (single shared file) was discarded due to lack of clear benefit and added complexity in filtering and validation.

## Notes

- Recipe IDs must be globally unique — duplicate IDs will cause a conflict error during loading
- The `inputs` and `outputs` arrays may contain multiple items for compound recipes
- Metadata (`ItemStack.metadata`) enables NBT-based crafting conditions
- Future extensions may add `priority` field for recipe ordering within the same machine