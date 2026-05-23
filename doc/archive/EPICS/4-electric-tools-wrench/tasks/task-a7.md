# Task A7: Client BatteryBuffer GUI

## Objective
Add an ImGui window for Battery Buffer blocks, showing EU storage level and tool charging slots with progress bars.

## Requirements

### 7.1 BatteryBufferWindow
**Location**: `src/services/game_client/ui/MachineWindow.cpp` or `BatteryBufferWindow.h` (NEW)

Extend the existing `MachineWindow` system (or create a dedicated window):

```cpp
class BatteryBufferWindow {
public:
    void Render(BatteryBufferState& state);
    bool IsOpen() const { return m_open; }
    void Open(const BlockPos& pos, uint8_t tier, uint8_t numSlots);
    void Close();
    
private:
    bool m_open = false;
    BlockPos m_pos;
    uint8_t m_tier;
    uint8_t m_numSlots;
    
    void renderEnergyBar(int32_t stored, int32_t capacity);
    void renderSlot(int idx, const ItemStack& item, int32_t itemEnergy, int32_t itemCapacity);
    void renderSlotGrid();
};
```

### 7.2 BatteryBuffer state from server
**Location**: `src/protocol/core.fbs` or `gateway.fbs`

Server sends BatteryBuffer state via BlockEntityUpdate (existing) or a new message:

```flatbuffers
// Extend BlockEntityUpdate or create new:
table BatteryBufferState {
    pos: Vec3i;
    tier: uint8;
    stored: uint32;
    capacity: uint32;
    slots: [ItemStack];           // items in charging slots
    slot_energy: [uint32];        // current energy per slot item
    slot_max_energy: [uint32];    // max capacity per slot item
}
```

### 7.3 GUI layout
```
┌──────────────────────────────┐
│  Battery Buffer LV           │
│  ┌────────────────────────┐  │
│  │ ████████░░░░░░░░░░ 40% │  │  ← Energy bar (stored/capacity)
│  │ 16,000 / 40,000 EU    │  │
│  └────────────────────────┘  │
│                              │
│  ┌──────┐                    │
│  │ Drill │  ████░░ 50%     │  │  ← Slot with item + charge bar
│  │ LV    │  2,000/4,000 EU │  │
│  └──────┘                    │
│  ┌──────┐                    │
│  │ (empty)                │  │  ← Empty slot (drop tool here)
│  └──────┘                    │
└──────────────────────────────┘
```

### 7.4 Client-server interaction
1. Player right-clicks Battery Buffer block
2. Client sends `ToolAction(CHARGE_ITEM, pos, slot_idx)`
3. Server opens battery buffer UI (or sends state via BlockEntityUpdate)
4. Client renders BatteryBufferWindow with current state
5. Each tick, server publishes updated state (energy, slot fill)
6. Client can drag items between inventory and buffer slots

### 7.5 InteractionSystem integration
**Location**: `src/services/game_client/World/InteractionSystem.cpp`

```cpp
void InteractionSystem::Update(...) {
    // ... existing logic ...
    
    // NEW: Right-click on battery buffer
    if (input.mouseRightPressed && hasHighlight_) {
        if (isBatteryBuffer(highlightedBlockType)) {
            // Open BatteryBufferWindow instead of normal block place
            m_batteryBufferWindow.Open(highlightedBlock_, getTier(highlightedBlockType), 
                                        getNumSlots(highlightedBlockType));
            return;
        }
    }
}
```

## Acceptance Criteria
- [ ] BatteryBufferWindow opens on right-click
- [ ] Shows stored/capacity energy bar
- [ ] Shows tool slots with charge progress
- [ ] Slot shows tool name + current/max energy
- [ ] State updates from server each tick (or via BlockEntityUpdate)
- [ ] Player can drag items in/out of buffer slots (via InventoryAction)
- [ ] Window closes on ESC or clicking outside

## Dependencies
- Task A6 (BatteryBuffer tick — server state)
- Existing MachineWindow/ImGui infrastructure
- InventoryAction (existing)

## Files to Create/Modify
- `src/services/game_client/ui/BatteryBufferWindow.h/.cpp` — NEW
- `src/services/game_client/World/InteractionSystem.cpp` — open trigger
- `src/protocol/core.fbs` — BatteryBufferState table
