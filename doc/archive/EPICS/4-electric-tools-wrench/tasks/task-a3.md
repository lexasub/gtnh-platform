# Task A3: ActionDispatcher — ToolAction Handler

## Objective
Add ToolAction handling to the existing ActionDispatcher in SimulationCore, routing tool actions to the appropriate systems.

## Requirements

### 3.1 Find and extend ActionDispatcher
**Location**: `src/services/simulation_core/main.cpp`

**Current** (from agent): ActionDispatcher handles PlayerAction, InventoryAction, SetBlockAction. Add ToolAction handler:

```cpp
class ActionDispatcher {
public:
    // ... existing handlers ...
    
    // NEW:
    void handleToolAction(const Protocol::ToolAction* action);
    
private:
    // Existing: casHandler, inventoryStore
    // Potentially new references:
    EnergySystem* m_energySystem = nullptr;
    WrenchHandler* m_wrenchHandler = nullptr;
};
```

### 3.2 ToolAction dispatch logic
**Location**: `src/services/simulation_core/main.cpp`

```cpp
// In the message router callback:
if (auto* toolAction = msg->payload_as_ToolAction()) {
    dispatcher.handleToolAction(toolAction);
    return;
}

// In ActionDispatcher::handleToolAction():
void ActionDispatcher::handleToolAction(const Protocol::ToolAction* action) {
    switch (action->action()) {
        case Protocol::ToolActionType_MINE_BLOCK:
            handleMineBlock(action);
            break;
        case Protocol::ToolActionType_WRENCH_CYCLE:
            handleWrenchCycle(action);
            break;
        case Protocol::ToolActionType_CHARGE_ITEM:
            handleChargeItem(action);
            break;
        case Protocol::ToolActionType_TOOL_INFO:
            handleToolInfo(action);
            break;
    }
}
```

### 3.3 MINE_BLOCK handler (stub for Task A6)
**Location**: `src/services/simulation_core/main.cpp`

```cpp
void ActionDispatcher::handleMineBlock(const Protocol::ToolAction* action) {
    // 1. Get player inventory → find tool in slot_idx
    // 2. Check tool has enough energy (EnergyStorage in item)
    //    (If energy < mining_cost → reject with "no_energy")
    // 3. Check tool tier >= block mining level
    // 4. Execute CAS: block → AIR
    // 5. Deduct energy from tool
    // 6. Send ToolActionResp with success + updated energy
    
    auto playerInv = inventoryStore->getInventory(action->player_id());
    auto* toolSlot = playerInv->getSlot(action->slot_idx());
    
    if (!toolSlot || toolSlot->item_id != action->item_id()) {
        sendToolError(action, "no_tool");
        return;
    }
    
    // Check energy (detail in Task A6)
    int32_t energy = getItemEnergy(toolSlot);
    if (energy <= 0) {
        sendToolError(action, "no_energy");
        return;
    }
    
    // CAS break
    auto result = casHandler(action->pos(), action->item_id(), 0);
    if (result.success) {
        deductItemEnergy(toolSlot, miningCost(action->item_id(), result.block_id));
        sendToolResponse(action, true, result.block_id, getItemEnergy(toolSlot));
    } else {
        sendToolError(action, "cannot_mine");
    }
}
```

### 3.4 WRENCH_CYCLE handler (stub for Task B4)
```cpp
void ActionDispatcher::handleWrenchCycle(const Protocol::ToolAction* action) {
    // Delegate to WrenchHandler (Task B4)
    // For now: log + return success
    spdlog::info("Wrench cycle at ({},{},{}) face={}", 
                 action->pos()->x(), action->pos()->y(), action->pos()->z(), action->face());
    sendToolResponse(action, true, 0, 0);
}
```

### 3.5 CHARGE_ITEM handler (stub for Task A7)
```cpp
void ActionDispatcher::handleChargeItem(const Protocol::ToolAction* action) {
    // Delegate to BatteryBufferSystem (Task A7)
    sendToolResponse(action, true, 0, 0);
}
```

### 3.6 Wire ToolAction from gateway
**Location**: `src/services/gateway/` or wherever incoming messages are routed

```cpp
// In gateway message handler:
// Forward ToolAction from client to SimulationCore
if (msg->payload_type() == GatewayPayload_ToolAction) {
    routerClient->publish("simcore.tool.action", data);
}
```

## Acceptance Criteria
- [ ] `handleToolAction()` dispatches on ToolActionType
- [ ] MINE_BLOCK handler checks tool existence + energy
- [ ] WRENCH_CYCLE handler creates proper response
- [ ] CHARGE_ITEM handler returns response
- [ ] ToolActionResp sent back to client
- [ ] Error responses for invalid actions
- [ ] Gateway forwards ToolAction to SimulationCore

## Dependencies
- Task A2 (ToolAction protocol)
- Required by: Task A6 (mining), Task B4 (wrench cycle)

## Files to Modify
- `src/services/simulation_core/main.cpp` — ActionDispatcher extensions
- `src/services/gateway/` — message routing for ToolAction
