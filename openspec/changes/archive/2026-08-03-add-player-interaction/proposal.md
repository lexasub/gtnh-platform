# Change: Add Player Interaction Specs

## Why
Userflow 01 describes CAS block placement, crafting pipeline, inventory drag-and-drop, machine UI interaction, and world exploration. Most features exist in code but lack formal openspec coverage. This change documents the existing implementation as a capability spec.

## What Changes
- Formalize CAS-based block placement spec (optimistic ack, conflict resolution)
- Formalize crafting pipeline spec (CraftRequest → RecipeManager → CraftResponse)
- Formalize inventory system spec (drag-and-drop, MetaDB persistence)
- Formalize machine interaction spec (MachineWindow, state queries)
- Formalize world exploration spec (chunk loading, generation)

## Impact
- Affected specs: player-interaction (new)
- Affected code: gateway (CAS handler), simulation_core (crafting, inventory), game_client (UI windows), meta_db (inventory persistence)
