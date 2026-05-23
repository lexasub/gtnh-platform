# Task 3: Add SetMachineSlotResp Handling in Client (MachineWindow.cpp)

## Objective
Implement SetMachineSlotResp handling in the game client to provide UI feedback for machine slot operations.

## Requirements

### 3.1 Update MachineWindow.cpp SendSetMachineSlot()
**Location**: `src/services/game_client/MachineWindow.cpp`

**Current Implementation**:
```cpp
void MachineWindow::SendSetMachineSlot(int slot_idx, ItemStack item, bool is_put) {
    PlayerAction action;
    action.type = PlayerActionType::SetMachineSlot;
    action.playerId = playerId;
    
    SetMachineSlotReq req;
    req.pos = machinePos;
    req.slot_idx = slot_idx;
    req.item_id = item.id;
    req.count = item.count;
    req.action = is_put ? ActionType::Put : ActionType::Take;
    
    gateway->sendRequest(action, req);
}
```

**Required Implementation**:
```cpp
void MachineWindow::SendSetMachineSlot(int slot_idx, ItemStack item, bool is_put) {
    PlayerAction action;
    action.type = PlayerActionType::SetMachineSlot;
    action.playerId = playerId;
    
    SetMachineSlotReq req;
    req.pos = machinePos;
    req.slot_idx = slot_idx;
    req.item_id = item.id;
    req.count = item.count;
    req.action = is_put ? ActionType::Put : ActionType::Take;
    
    // Add response callback
    gateway->sendRequestWithCallback(
        action, 
        req,
        [this, slot_idx](const SetMachineSlotResp& resp) {
            OnSetMachineSlotResponse(resp, slot_idx);
        }
    );
    
    // Keep optimistic UI update (for smooth UX)
    UpdateSlotUI(slot_idx, item, is_put);
}
```

### 3.2 Add OnSetMachineSlotResponse() handler
**Location**: `src/services/game_client/MachineWindow.cpp`

**Implementation**:
```cpp
void MachineWindow::OnSetMachineSlotResponse(const SetMachineSlotResp& resp, int slot_idx) {
    if (!resp.success) {
        // Operation failed - show error
        ShowErrorMessage(resp.error);
        // Revert slot UI to previous state
        RevertSlotUI(slot_idx);
    } else {
        // Operation successful - update slot with actual result
        UpdateSlotWithResponse(resp.slot_idx, resp.item);
        ShowSuccessFeedback();
    }
}
```

### 3.3 Add UI feedback functions
**Location**: `src/services/game_client/MachineWindow.cpp`

**Implementation**:
```cpp
void MachineWindow::ShowErrorMessage(const std::string& error) {
    // Show error toast/notification
    ImGui::OpenPopup("Error");
    errorMessage = error;
}

void MachineWindow::RevertSlotUI(int slot_idx) {
    // Reload current inventory from server state
    RefreshMachineInventory();
}

void MachineWindow::UpdateSlotWithResponse(int slot_idx, const ItemStack& item) {
    // Update slot with actual server state (not optimistic UI)
    slots[slot_idx] = item;
    ImGui::GetWindowDrawList()->AddText(...); // Visual feedback
}
```

### 3.4 Add message handling in GameClient
**Location**: `src/services/game_client/GameClient.cpp` or similar

**Implementation**:
```cpp
// In message processing loop:
if (msg.type == GatewayMsg::kSetMachineSlotResp) {
    auto& resp = std::get<SetMachineSlotResp>(msg.data);
    machineWindow->OnSetMachineSlotResponse(resp, resp.slot_idx);
}
```

## UI/UX Requirements

### 3.5 Error Display
- Show error messages in UI (toast, popup, or status bar)
- Different colors for different error types
- Allow user to retry failed operations

### 3.6 Success Feedback
- Visual confirmation of slot update (highlight, animation)
- Sound feedback for successful operations
- Update slot count and item details accurately

### 3.7 Inventory Refresh
- On error, reload entire inventory from server state
- Prevent UI desync with server state
- Smooth rollback on failed operations

## Evidence Requirements
- [ ] MachineWindow.cpp has OnSetMachineSlotResponse handler
- [ ] Error cases display appropriate messages
- [ ] Success cases update UI with server state
- [ ] Gateway integration for response callbacks works
- [ ] UI feedback provides clear user guidance

## Dependencies
- Gateway message handling must be implemented (Task 1 & 2)
- UI framework (ImGui) must be available for error display
- Response callback system must exist in gateway

## Testing
- Client should show success feedback for valid operations
- Error cases should display appropriate messages
- UI should reflect server state accurately
- Rollback should work on failed operations

---