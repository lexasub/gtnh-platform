# Task: Dissociation (Multiblock Destruction)

## Overview
Implement multiblock dissociation system where breaking a multiblock's anchor block destroys the entire multiblock. This includes cleanup, resource recovery, and client-side visual updates.

## Goal
- Implement anchor block detection for multiblock identification
- Create dissociation cascade for complete multiblock destruction
- Clear mb_id from all pattern blocks
- Eject hatch contents (items, fluids, energy)
- Publish multiblock destruction events
- Update client-side multiblock visuals
- Integrate with EntityStateStore for persistence

## Acceptance Criteria
- [ ] Anchor block detection functional
- [ ] Dissociation cascade implemented
- [ ] mb_id cleared from all pattern blocks
- [ ] Hatch contents ejection working
- [ ] Multiblock destruction events published
- [ ] Client-side visual updates
- [ ] Integration with EntityStateStore
- [ ] Unit tests for dissociation scenarios
- [ ] Edge case handling (partial patterns)

## Requirements

### Technical Requirements
- **Language**: C++
- **Location**: `src/services/simulation_core/ECS/systems/`
- **Integration**: With PatternRegistry, EntityStateStore, MessageRouter
- **Performance**: Fast dissociation for large multiblocks

### Dissociation System Architecture
```cpp
class DissociationSystem {
public:
    // Check if position is multiblock anchor
    bool IsMultiblockAnchor(uint32_t x, uint32_t y, uint32_t z) const;
    
    // Get multiblock controller for anchor position
    const MultiblockController* GetControllerForAnchor(uint32_t x, uint32_t y, uint32_t z) const;
    
    // Trigger dissociation cascade
    void TriggerDissociation(uint32_t x, uint32_t y, uint32_t z);
    
    // Dissociate multiblock by controller
    void DissociateMultiblock(const MultiblockController& controller);
    
    // Clear mb_id from pattern block
    void ClearMbIdFromBlock(uint32_t x, uint32_t y, uint32_t z);
    
    // Eject contents from hatch
    void EjectHatchContents(const MultiblockController& controller);
};
```

### Dissociation Data Structures
```cpp
struct DissociationResult {
    uint64_t controller_id;     // ID of destroyed controller
    uint32_t blocks_dissociated; // Number of blocks destroyed
    uint32_t items_recovered;    // Items recovered from hatches
    uint32_t fluids_recovered;   // Fluids recovered from hatches
    std::vector<ItemStack> ejected_items;
    std::vector<FluidStack> ejected_fluids;
    bool was_successful;          // Whether dissociation completed
};
```

## Implementation Details

### Anchor Detection
```cpp
bool DissociationSystem::IsMultiblockAnchor(uint32_t x, uint32_t y, uint32_t z) const {
    // Check if this block has a controller reference
    ChunkData chunk = chunkStore.GetChunk(x, y, z);
    if (!chunk.is_loaded) {
        return false;
    }
    
    // Check meta-layer for mb_id
    uint64_t mb_id = chunk.GetMbId(x, y, z);
    if (mb_id == 0) {
        return false;
    }
    
    // Verify this position is a controller anchor
    const MultiblockPattern* pattern = patternRegistry.GetPattern(mb_id);
    if (!pattern) {
        return false;
    }
    
    // Check if position matches controller anchor
    for (const auto& controller_pos : pattern->controller_pos) {
        BlockPos rel_pos = {
            .x = x - chunk.x,
            .y = y - chunk.y, 
            .z = z - chunk.z
        };
        
        if (rel_pos.x == controller_pos.x &&
            rel_pos.y == controller_pos.y &&
            rel_pos.z == controller_pos.z) {
            return true;
        }
    }
    
    return false;
}
```

### Dissociation Cascade
```cpp
void DissociationSystem::TriggerDissociation(uint32_t x, uint32_t y, uint32_t z) {
    if (!IsMultiblockAnchor(x, y, z)) {
        return; // Not a multiblock anchor
    }
    
    const MultiblockController* controller = GetControllerForAnchor(x, y, z);
    if (!controller) {
        return; // No controller found
    }
    
    DissociateMultiblock(*controller);
}

void DissociationSystem::DissociateMultiblock(const MultiblockController& controller) {
    DissociationResult result;
    result.controller_id = controller.id;
    
    // Get pattern for this multiblock
    const MultiblockPattern* pattern = patternRegistry.GetPattern(controller.id);
    if (!pattern) {
        return; // No pattern found
    }
    
    // Find all blocks in this multiblock
    std::vector<BlockPos> multiblock_blocks = CalculateMultiblockBlocks(controller, *pattern);
    result.blocks_dissociated = multiblock_blocks.size();
    
    // Clear mb_id from all pattern blocks
    for (const auto& block_pos : multiblock_blocks) {
        ClearMbIdFromBlock(block_pos.x, block_pos.y, block_pos.z);
    }
    
    // Eject hatch contents
    std::vector<ItemStack> items = EjectHatchContents(controller);
    std::vector<FluidStack> fluids = EjectFluidHatches(controller);
    result.ejected_items = items;
    result.ejected_fluids = fluids;
    result.items_recovered = items.size();
    result.fluids_recovered = fluids.size();
    
    // Remove controller entity
    RemoveControllerEntity(controller.id);
    
    // Publish destruction event
    PublishDissociationEvent(result);
    
    // Store dissociation result for persistence
    PersistDissociationResult(result);
}
```

### Pattern Block Calculation
```cpp
cpp
std::vector<BlockPos> DissociationSystem::CalculateMultiblockBlocks(
    const MultiblockController& controller,
    const MultiblockPattern& pattern) const {
    
    std::vector<BlockPos> blocks;
    
    // Calculate all blocks in the multiblock pattern
    for (int32_t dx = -pattern.size_x/2; dx <= pattern.size_x/2; dx++) {
        for (int32_t dy = -pattern.size_y/2; dy <= pattern.size_y/2; dy++) {
            for (int32_t dz = -pattern.size_z/2; dz <= pattern.size_z/2; dz++) {
                // Check if this offset is part of the pattern
                if (IsBlockInPattern(dx, dy, dz, pattern)) {
                    blocks.push_back({
                        .x = controller.x + dx,
                        .y = controller.y + dy,
                        .z = controller.z + dz
                    });
                }
            }
        }
    }
    
    return blocks;
}
```

### Hatch Ejection
```cpp
std::vector<ItemStack> DissociationSystem::EjectHatchContents(
    const MultiblockController& controller) {
    
    std::vector<ItemStack> ejected_items;
    
    // Get all hatches for this controller
    std::vector<HatchDef> hatches = GetHatchesForController(controller);
    
    for (const auto& hatch : hatches) {
        switch (hatch.type) {
            case HatchDef::Type::INPUT:
            case HatchDef::Type::OUTPUT:
                // Eject items from item hatches
                ItemStack item = GetItemFromHatch(hatch.position);
                if (!item.isEmpty()) {
                    ejected_items.push_back(item);
                    ConsumeItem(hatch.position, item);
                }
                break;
                
            case HatchDef::Type::FLUID:
                // Eject fluids from fluid hatches
                FluidStack fluid = GetFluidFromHatch(hatch.position);
                if (!fluid.isEmpty()) {
                    ejected_items.push_back(ItemStack{ // Convert to item?
                        .id = ITEM_BUCKET_WATER,
                        .count = fluid.amount / FLUID_PER_ITEM
                    });
                    ConsumeFluid(hatch.position, fluid);
                }
                break;
                
            case HatchDef::Type::ENERGY:
                // Energy hatches have no ejectable contents
                break;
                
            case HatchDef::Type::MUFFLER:
                // Muffler hatches have no ejectable contents
                break;
        }
    }
    
    return ejected_items;
}
```

### Event Publishing
```cpp
void DissociationSystem::PublishDissociationEvent(const DissociationResult& result) {
    // Create dissociation event message
    MultiblockDissociationEvent event = {
        .controller_id = result.controller_id,
        .blocks_dissociated = result.blocks_dissociated,
        .items_recovered = result.items_recovered,
        .fluids_recovered = result.fluids_recovered,
        .timestamp = GetCurrentTimestamp()
    };
    
    // Publish to MessageRouter
    messageRouter.Publish(
        "sim.multiblock.dissociated",
        event
    );
}
```

## Integration Points

### With EntityStateStore
```cpp
void DissociationSystem::PersistDissociationResult(const DissociationResult& result) {
    // Store dissociation result for persistence
    DissociationState state = {
        .controller_id = result.controller_id,
        .dissociation_time = GetCurrentTimestamp(),
        .blocks_dissociated = result.blocks_dissociated,
        .items_recovered = result.items_recovered,
        .fluids_recovered = result.fluids_recovered,
        .ejected_items = result.ejected_items,
        .ejected_fluids = result.ejected_fluids,
        .was_successful = result.was_successful
    };
    
    entityStateStore.SetEntityState(
        ENTITY_DISSOCIATION_RESULT,
        state
    );
}
```

### With Client
```cpp
// Notify client of multiblock destruction
void DissociationSystem::NotifyClientDissociation(const DissociationResult& result) {
    MultiblockDestructionPacket packet = {
        .controller_id = result.controller_id,
        .blocks_dissociated = result.blocks_dissociated,
        .items_recovered = result.items_recovered,
        .fluids_recovered = result.fluids_recovered,
        .timestamp = GetCurrentTimestamp()
    };
    
    // Send to all clients in the same dimension
    messageRouter.PublishToDimension(
        "client.multiblock.destroyed",
        packet
    );
}
```

## Edge Cases and Error Handling

### Partial Pattern Dissociation
```cpp
void DissociationSystem::HandlePartialPatternDissociation(
    uint32_t x, uint32_t y, uint32_t z) {
    
    // Check if this is part of a multiblock
    uint64_t mb_id = chunkStore.GetMbId(x, y, z);
    if (mb_id == 0) {
        return; // Not part of multiblock
    }
    
    // Check if controller is intact
    const MultiblockController* controller = GetControllerForAnchor(x, y, z);
    if (controller) {
        // Controller is intact, full dissociation will happen soon
        return;
    }
    
    // Controller is destroyed, clear this block's mb_id
    ClearMbIdFromBlock(x, y, z);
    
    // Eject contents if any
    EjectContentsFromBlock(x, y, z);
}
```

### Concurrent Dissociation
```cpp
void DissociationSystem::HandleConcurrentDissociation() {
    // Lock controller before dissociation
    std::lock_guard<std::mutex> lock(dissociation_mutex);
    
    // Process all pending dissociations
    while (!pending_dissociations.empty()) {
        auto dissociation = pending_dissociations.front();
        pending_dissociations.pop();
        
        // Skip if controller already destroyed
        if (IsControllerDestroyed(dissociation.controller_id)) {
            continue;
        }
        
        // Perform dissociation
        PerformDissociation(dissociation);
    }
}
```

## Files to Modify
- `src/services/simulation_core/ECS/systems/DissociationSystem.h` - New
- `src/services/simulation_core/ECS/systems/DissociationSystem.cpp` - New
- `src/services/simulation_core/ECS/components/DissociationResult.h` - New
- `src/services/simulation_core/src/PatternMatcher.cpp` - Updated
- `src/services/simulation_core/src/SimulationEngine.cpp` - Updated
- `src/services/message_router/dissociation.go` - New (Go sidecar)

## Testing Strategy
- Unit tests for anchor detection
- Integration tests for dissociation cascade
- Edge case tests (partial patterns)
- Event publishing tests
- Client notification tests
- Concurrent dissociation tests

## Success Metrics
- Anchor detection 100% accurate
- Dissociation cascade complete
- Hatch contents ejected properly
- Events published correctly
- Client visuals updated
- Partial pattern handling
- Concurrent dissociation safe