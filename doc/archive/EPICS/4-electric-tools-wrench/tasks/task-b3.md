# Task B3: Client Wrench Interaction (Raycast Face Detection)

## Objective
When the player right-clicks with a wrench on a machine, detect which face of the block was hit via raycast and send MachineAction(WRENCH_CYCLE) to the server.

## Requirements

### 3.1 Raycast face detection
**Location**: `src/services/game_client/World/InteractionSystem.cpp`

**Current** (from agent): right-click places a block from inventory. **Change**: if holding wrench, send WRENCH_CYCLE instead.

```cpp
void InteractionSystem::Update(const Camera& camera, const InputState& input,
                                World& world, NetClient& netClient) {
    // ... existing raycast logic ...
    
    // NEW: Right-click with wrench
    if (input.mouseRightPressed && hasHighlight_ && isWrench(selectedItemId)) {
        // Detect which face was hit by raycast
        Face hitFace = getRaycastFace(camera, highlightedBlock_);
        
        // Send WRENCH_CYCLE
        netClient.SendMachineAction(
            MachineActionType::WRENCH_CYCLE,
            highlightedBlock_.x, highlightedBlock_.y, highlightedBlock_.z,
            static_cast<uint8_t>(hitFace),
            player_id);
        return;  // don't place block
    }
    
    // Existing: right-click place block
    if (input.mouseRightPressed && hasHighlight_) {
        // ... existing place logic ...
    }
}
```

### 3.2 Face detection from raycast
```cpp
// Determine which face of a block the raycast hit
Face getRaycastFace(const Camera& camera, const BlockPos& block) {
    // 1. Get the ray from camera through cursor
    // 2. Find the intersection point on the block surface
    // 3. Compare intersection normal to determine face:
    //    normal ≈ (0,-1,0) → DOWN
    //    normal ≈ (0,1,0)  → UP
    //    normal ≈ (0,0,-1) → NORTH
    //    normal ≈ (0,0,1)  → SOUTH
    //    normal ≈ (-1,0,0) → WEST
    //    normal ≈ (1,0,0)  → EAST
    
    // Reuse existing raycast from block interaction (already computes hit point)
    auto hit = raycast(camera, MAX_REACH);
    if (!hit) return Face::UP;  // fallback
    
    // Get the normal of the face that was hit
    glm::ivec3 normal = hit->normal;
    
    if (normal.y == -1) return Face::DOWN;
    if (normal.y == 1)  return Face::UP;
    if (normal.z == -1) return Face::NORTH;
    if (normal.z == 1)  return Face::SOUTH;
    if (normal.x == -1) return Face::WEST;
    if (normal.x == 1)  return Face::EAST;
    
    return Face::UP;  // fallback
}
```

### 3.3 Wrench tool detection
```cpp
// Is this item a wrench?
bool isWrench(uint16_t itemId) {
    return itemId == ITEM_WRENCH;  // 95
}
```

### 3.4 NetClient::SendMachineAction
**Location**: `src/services/game_client/Network/NetClient.h/.cpp`

```cpp
void NetClient::SendMachineAction(MachineActionType action, int32_t x, int32_t y, int32_t z, 
                                    uint8_t face, uint64_t playerId) {
    flatbuffers::FlatBufferBuilder builder(64);
    
    auto pos = Protocol::CreateVec3i(builder, x, y, z);
    auto msg = Protocol::CreateMachineAction(builder, playerId, 
        static_cast<Protocol::MachineActionType>(action), pos, face, 0);
    
    // Wrap in gateway message and send
    // ... (follow same pattern as SendBlockAction)
}
```

### 3.5 Visual feedback
When WRENCH_CYCLE succeeds:
- Brief highlight on the changed face (green tint, 200ms)
- Sound: metallic click
- Tooltip briefly shows new role name: "Face: INPUT"

## Acceptance Criteria
- [ ] Right-click with wrench on machine → detects hit face
- [ ] Sends MachineAction(WRENCH_CYCLE, pos, face) to server
- [ ] Right-click with wrench on non-machine → normal block place
- [ ] Right-click with non-wrench item → normal block place
- [ ] Face detection works from any camera angle
- [ ] Fallback: if face detection ambiguous, default to UP face
- [ ] `SendMachineAction()` creates valid FlatBuffer

## Dependencies
- Task A1 (ITEM_WRENCH = 95)
- Task B2 (MachineAction protocol)
- Existing InteractionSystem + raycast
- Required by: B4 (handler), B7 (visual update)

## Files to Modify
- `src/services/game_client/World/InteractionSystem.cpp` — wrench right-click handler
- `src/services/game_client/Network/NetClient.h/.cpp` — SendMachineAction()
