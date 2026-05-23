# Task A8: Client Tool Charge Tooltip

## Objective
Show the current EU charge of electric tools as a tooltip when hovering over them in the inventory or machine slots.

## Requirements

### 8.1 Tooltip rendering
**Location**: `src/services/game_client/ui/ItemTooltip.cpp` (or similar)

Extend existing item tooltip to show energy for tools:

```cpp
void renderTooltip(uint16_t itemId, uint16_t meta) {
    // ... existing tooltip (name, description) ...
    
    // NEW: Show energy for electric tools
    auto it = TOOL_ENERGY_DEFS.find(itemId);
    if (it != TOOL_ENERGY_DEFS.end()) {
        const auto& def = it->second;
        int32_t energy = meta;  // meta = current energy
        float pct = (def.capacity > 0) ? (100.0f * energy / def.capacity) : 0.0f;
        
        ImGui::Separator();
        ImGui::TextColored(energyColor(pct), "EU: %d / %d", energy, def.capacity);
        
        // Mini progress bar
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, energyBarColor(pct));
        ImGui::ProgressBar(pct / 100.0f, ImVec2(100, 6));
        ImGui::PopStyleColor();
        
        // Tier indicator
        const char* tierNames[] = {"ULV", "LV", "MV", "HV"};
        if (def.tier < 4) {
            ImGui::Text("Tier: %s", tierNames[def.tier]);
        }
    }
}

// Color by charge level
ImVec4 energyColor(float pct) {
    if (pct > 50) return ImVec4(0.2f, 0.8f, 0.2f, 1);  // green
    if (pct > 20) return ImVec4(0.8f, 0.8f, 0.2f, 1);  // yellow
    return ImVec4(0.8f, 0.2f, 0.2f, 1);                  // red
}

ImVec4 energyBarColor(float pct) {
    if (pct > 50) return ImVec4(0.0f, 0.6f, 0.0f, 1);
    if (pct > 20) return ImVec4(0.6f, 0.6f, 0.0f, 1);
    return ImVec4(0.6f, 0.0f, 0.0f, 1);
}
```

### 8.2 Tooltip in hotbar
When hovering over a tool in the hotbar/action bar:
- Show simplified tooltip: "Drill LV [████░░] 50%"
- Use existing hotbar rendering

### 8.3 Tooltip in machine slots
When tool is placed in a machine input slot:
- Show full tooltip with energy bar
- Same as inventory tooltip

### 8.4 Item stack meta propagation
The tool's energy (stored in `meta`) must be sent from server to client as part of ItemStack in all inventory/grid messages:
- BlockEntityUpdate (machine slots)
- InventoryAction response (player inventory)
- BatteryBuffer state (Task A7)

**Verify**: `ItemStack` in `core.fbs` includes `meta` field.

## Acceptance Criteria
- [ ] Hovering over drill in inventory shows "EU: 2000/4000" with colored bar
- [ ] Color changes: green >50%, yellow 20-50%, red <20%
- [ ] Tooltip shows tier name (ULV/LV/MV/HV)
- [ ] Hotbar shows mini energy indicator
- [ ] Machine slot tooltips also show energy
- [ ] Energy value updates when tool is charged/used
- [ ] Non-tool items show no energy tooltip

## Dependencies
- Task A4 (ItemEnergyStorage — energy in meta)
- Existing ItemStack protocol (meta field)
- ImGui tooltip system

## Files to Modify
- `src/services/game_client/ui/ItemTooltip.cpp` — energy tooltip render
- `src/services/game_client/ui/Hotbar.cpp` — mini energy indicator
