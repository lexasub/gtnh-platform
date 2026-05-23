# Machine Client GUI

**Layer 1** + **Layer 2**  
**Status**: Draft  
**Epic**: 1-gameplay-machines-multiblocks

## Affected Services

| Service | Role |
|---------|------|
| **GameClient** | Primary — ImGui window, slot rendering, progress bar |
| **Gateway** | Relay — forwards BlockEntityUpdate to client |

---

## Overview

The Machine Client GUI renders an ImGui window when the player right-clicks a machine block. It displays the machines current inventory slots and processing progress. The window matches the existing GameClient pattern using bgfx for rendering, GLFW for input, and ImGui for the UI.

---

## GUI Layout

### Slots

- **Input Slots**: Two slots on the left side
- **Output Slot**: One slot on the right side
- **Progress Bar**: Horizontal bar below slots, shows 0-100% of current recipe

### ImGui Pattern

The window follows existing GameClient conventions:
- Window title bar with machine name and coordinates
- Centered layout using ImGui columns
- Icons rendered for items in slots
- Progress bar uses ImGui::ProgressBar

---

## Interaction Model

### Opening the GUI

Right-clicking a machine block opens the GUI. The window remains open until:
- The player opens a different block
- The player closes the window with Escape
- The player moves to a new chunk

### Closing the GUI

Pressing Escape or opening another block closes the window and returns the player to standard FPS controls.

---

## Data Synchronization

### BlockEntityUpdate

Updates come from the BlockEntityUpdate message. The message contains:
- x, y, z coordinates of the machine
- Machine type identifier
- MachineState (recipe progress)
- MachineInventory (input/output slots)
- EnergyStorage (current and maximum energy)

The GUI consumes these updates and refreshes the ImGui window contents.

---

## File Locations

| File | Path |
|------|------|
| GUI rendering | `src/services/game_client/gui/machine_window.cpp` |
| Data structures | `src/services/game_client/gui/machine_window.h` |
| ImGui integration | `src/services/game_client/gui/ImGuiRenderer.cpp` |
| BlockEntityUpdate schema | `src/protocol/BlockEntityUpdate.fbs` |

---

## Acceptance Criteria

#### Scenario 1: Opening Machine GUI

Player right-clicks a furnace at coordinates 100, 64, 50. The ImGui window appears with:
- Title bar reading "Furnace (100, 64, 50)"
- Two empty input slots on the left
- One empty output slot on the right
- Progress bar at 0%
- No items displayed in slots

#### Scenario 2: Updating Machine State

Player places coal and a dirt block into the input slots. The GUI refreshes to show:
- Coal item in input slot 0
- Dirt block in input slot 1
- Progress bar advancing to 50%
- Output slot remains empty until processing completes

#### Scenario 3: Multiblock Machine GUI

Player assembles a steam boiler multiblock pattern. The GUI displays:
- Combined inventory from all boiler components
- Total energy across the entire multiblock
- Progress bar reflecting the multiblock processing state
- Machine title showing "Steam Boiler (Multiblock)"

---

## Open Questions

- Should the GUI support drag-and-drop item insertion from the player inventory?
- What is the exact visual representation for empty slots in the GUI?
- Should the GUI display machine energy level as a secondary bar alongside the progress bar?
