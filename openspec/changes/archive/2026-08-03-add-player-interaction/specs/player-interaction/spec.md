## ADDED Requirements

### Requirement: CAS Block Placement
The system SHALL use Compare-And-Swap (CAS) semantics for block placement and breaking to prevent race conditions between concurrent players.

#### Scenario: Optimistic placement accepted
- **GIVEN** a player breaks or places a block at position (x,y,z)
- **WHEN** the client sends `SetBlockAction` (GatewayMsg::kSetBlockAction=11) with `expected_block_id` set to the block id the client currently sees at (x,y,z), `new_block_id` (0 = break), and a client-generated `request_id`
- **THEN** SimulationCore SHALL publish an optimistic `BlockAck(status=ACCEPTED)` immediately, before the CAS RPC completes
- **AND** the client SHALL mark the position as pending via `World::MarkBlockActionSent` to debounce duplicate sends

#### Scenario: CAS succeeds and block change is broadcast
- **GIVEN** SimulationCore sent `SetBlockCASReq` (pos, expected_block_id, new_block_id, meta) to ChunkStore
- **WHEN** ChunkStore returns `SetBlockCASResp` with `CASStatus=OK`
- **THEN** SimulationCore SHALL publish a `BlockChangedEvent` (carrying `source_player_id`) on topic `world.blocks.changed`
- **AND** Gateway SHALL forward it to other players as `BlockUpdate` (kBlockUpdate=4) but skip the source player (it already received the optimistic ack)
- **AND** for a break action the broken block SHALL be added to the player inventory via `onGiveItem` and the drill SHALL be consumed via `onDrillUse`

#### Scenario: CAS conflict triggers revert
- **GIVEN** another player changed the block at (x,y,z) after the first player read it
- **WHEN** ChunkStore returns `SetBlockCASResp` with `CASStatus=CONFLICT` and the `actual_block_id`
- **THEN** SimulationCore SHALL publish a second `BlockAck(status=CONFLICT)` carrying the actual block id
- **AND** the client SHALL revert its local world state to the actual block id via `OnBlockUpdate`

#### Scenario: Placing air is rejected
- **GIVEN** a player sends a placement action (RIGHT_MOUSE_CLICK) with `new_block_id=0`
- **WHEN** SimulationCore processes the `SetBlockAction`
- **THEN** it SHALL publish `BlockAck(status=REJECTED)` with reason "Cannot place air"
- **AND** SHALL NOT perform a CAS RPC

#### Scenario: Right-click on a machine opens interaction instead of mutating
- **GIVEN** a player right-clicks a block whose id is a registered machine
- **WHEN** SimulationCore receives the `SetBlockAction` with `RIGHT_MOUSE_CLICK`
- **THEN** it SHALL invoke `onMachineInteracted` and publish `BlockAck(status=ACCEPTED)`
- **AND** SHALL NOT send a CAS RPC, because machine interaction is not a block mutation

### Requirement: Crafting Pipeline
The system SHALL process workbench crafting requests through RecipeManager with server-authoritative inventory deduction.

#### Scenario: Valid recipe found and crafted
- **GIVEN** a player submits a `CraftRequest` (kCraftRequest=9) with a 3×3 grid of `ItemStack` slots from the workbench
- **WHEN** Gateway relays it on topic `sim.craft.request` and `CraftRequestHandler` runs
- **THEN** RecipeManager SHALL find the best matching recipe via `findRecipeByInputs(kCraftingTableMachineId=14, grid)`
- **AND** the recipe SHALL be crafted via `Recipe::craft(grid)` (inputs consumed, outputs stacked up to 64)
- **AND** consumed counts SHALL be deducted from the player inventory via `PlayerInventoryStore::setSlots`
- **AND** the result SHALL be added via `PlayerInventoryStore::giveItem`
- **AND** `CraftResponse` (kCraftResponse=10) with `success=true`, `result`, and the consumed `grid` SHALL be published on `sim.craft.response` and relayed to the client

#### Scenario: No recipe matches
- **GIVEN** a player submits a `CraftRequest` whose grid matches no recipe
- **WHEN** `findRecipeByInputs` returns null
- **THEN** `CraftResponse` with `success=false` and error "No matching recipe" SHALL be returned
- **AND** no inventory SHALL be deducted

#### Scenario: Craft triggers quest detection
- **GIVEN** a craft succeeds
- **WHEN** the result item is added to the player inventory
- **THEN** `QuestManager::checkCraftCompletion` SHALL be called with the crafted item id and count

#### Scenario: Machine recipes process over time
- **GIVEN** a player places input items into a machine (macerator, furnace, compressor, alloy_smelter, extractor, mixer, etc.) via `SetMachineSlotReq`
- **WHEN** `CheckRecipeReq` matches a recipe for that machine id
- **THEN** the machine SHALL process the recipe for its `duration` (ticks)
- **AND** on completion a `RecipeCompleted` event SHALL be published on topic `recipe.completed` carrying the full `result_slots`
- **AND** `RecipeCompletedHandler` SHALL replace the machine's `InventoryContainer` slots with `result_slots`

#### Scenario: Recipes loaded from YAML files
- **GIVEN** RecipeManager starts
- **WHEN** it loads `data/recipes/*.yaml`
- **THEN** each recipe SHALL define: name, machine class (e.g. macerator), optional min/max tier, optional energy_in filter, inputs (item, count, consume, replace), outputs (item, count, meta), duration (ticks), eu cost, and optional conditions
- **AND** recipes SHALL be matched by item_id, meta, and count >= required

### Requirement: Inventory Drag-and-Drop
The system SHALL provide a drag-and-drop inventory state machine in the client (DragManager) with server-side application via `InventoryAction`.

#### Scenario: Pickup transitions to Holding
- **GIVEN** DragManager is in Idle state
- **WHEN** the player left-clicks a non-empty slot
- **THEN** DragManager SHALL pick up the entire stack and transition to Holding
- **AND** right-click SHALL pick up `ceil(count/2)` and send `kActionSplit` (1)
- **AND** shift-click SHALL quick-move the entire stack and send `kActionQuickMove` (3)

#### Scenario: Place, merge, and swap while holding
- **GIVEN** DragManager is in Holding state
- **WHEN** the player left-clicks an empty slot
- **THEN** the held stack SHALL be placed there and `kActionMove` (0) SHALL be sent
- **AND** left-click on a same-item non-full slot SHALL merge stacks (up to 64) and send `kActionMove` (0)
- **AND** left-click on a different-item slot SHALL swap the held and target stacks and send `kActionMove` (0)

#### Scenario: Drop and cancel
- **GIVEN** the player is holding an item
- **WHEN** the player presses Q
- **THEN** the held item SHALL be dropped/destroyed and `kActionDrop` (2) SHALL be sent
- **AND** pressing ESC SHALL return the item to its source slot without a network action
- **AND** Q while hovering a slot (not dragging) SHALL drop that slot's item

#### Scenario: Server applies inventory action
- **GIVEN** the client sends `InventoryAction` (kInventoryAction=7) with `action_type` (0=MOVE, 1=SPLIT, 2=DROP), `source_slot`, `target_slot`, `count`
- **WHEN** Gateway relays it on topic `player.inventory.actions` and `InventoryActionHandler` runs
- **THEN** the action SHALL be applied to the player's 40-slot inventory in `PlayerInventoryStore`
- **AND** the resulting `InventoryUpdate` SHALL be published on `player.inventory.update` and relayed to the client as `kInventoryUpdate` (6)

### Requirement: Inventory Persistence
The system SHALL persist player inventory in MetaDB SQLite per mutation and load it on login.

#### Scenario: Inventory saved on every mutation
- **GIVEN** SimulationCore mutates a player's inventory via `setSlots` or `giveItem`
- **WHEN** the `onChange` callback fires
- **THEN** a `SetInventorySlotReq` SHALL be published on topic `meta_db.inventory.set`
- **AND** MetaDB SHALL upsert the slot into the `inventory` table (player_id, slot, block_id, count)
- **AND** the `onChange` callback SHALL NOT run for the same mutation twice

#### Scenario: Inventory loaded on login
- **GIVEN** a player connects and Gateway publishes `player.joined`
- **WHEN** MetaDB's `handlePlayerJoined` runs
- **THEN** it SHALL read the player's inventory from SQLite and publish it as an `InventoryUpdate` on topic `player.inventory.load`
- **AND** SimulationCore's `InventoryLoadHandler` SHALL apply it to `PlayerInventoryStore` via `applyUpdate`
- **AND** the inventory SHALL be re-published on `player.inventory.update` so the client receives it

#### Scenario: No explicit save on logout
- **GIVEN** a player disconnects and Gateway publishes `player.left`
- **WHEN** MetaDB's `handlePlayerLeft` runs
- **THEN** it SHALL save only the player position
- **AND** SHALL NOT re-save inventory, because inventory is already persisted per mutation

### Requirement: Machine Window UI
The system SHALL display machine state in a data-driven window, fed by pushed `BlockEntityUpdate` messages.

#### Scenario: Machine window opens on right-click
- **GIVEN** a player right-clicks a machine block
- **WHEN** the client raycasts the block and calls `UIDefaults::TryOpenBlockUI`
- **THEN** the window SHALL open locally via `BlockUIFactory::FindOrCreateMachine` — no network message is sent to "open" the machine
- **AND** machine state SHALL arrive asynchronously via `BlockEntityUpdate` pushes

#### Scenario: Machine window renders data-driven state
- **GIVEN** a `MachineWindow` is open and has received a `BlockEntityUpdate`
- **WHEN** the window renders
- **THEN** it SHALL show: input slots (count from MachineRegistry `slots_in`), output slots (`slots_out`), a progress bar (style by machine_class: ARROW/SPINNER/FLAME/GENERIC), and an energy bar (color-coded by EnergyType)
- **AND** it SHALL show an out-of-sync warning if no update was received for 30+ frames

#### Scenario: Machine state pushed via BlockEntityUpdate
- **GIVEN** a machine is running
- **WHEN** `MachineSystem::tick()` produces an update
- **THEN** SimulationCore SHALL publish `BlockEntityUpdate` (pos, machine_type, progress, energy, energy_capacity, energy_type, input_items, output_items, temperature, mb_id, structure_valid, hatches, covers) on topic `world.block_entity.update`
- **AND** Gateway SHALL relay it to the client as `kBlockEntityUpdate` (8)
- **AND** the client SHALL route it to the open window matching the position

### Requirement: Machine Slot and Action Protocol
The system SHALL support machine inventory transfers via `SetMachineSlotReq/Resp` and define `MachineAction` for config changes.

#### Scenario: Machine slot transfer
- **GIVEN** a player moves an item between player inventory and a machine window slot
- **WHEN** the client sends `SetMachineSlotReq` (kSetMachineSlot=15) with pos, slot_index, item_id, count, meta, and player_slot (255 = cursor)
- **THEN** Gateway SHALL relay it on topic `player.machine.slot`
- **AND** `MachineSlotHandler` SHALL update the ECS `InventoryContainer` and persist via EntityStateStore
- **AND** a `SetMachineSlotResp` (kSetMachineSlotResp=16) SHALL be returned to the client
- **AND** a fresh `BlockEntityUpdate` SHALL refresh the window

#### Scenario: MachineAction message defined in protocol
- **GIVEN** the protocol schema
- **THEN** `MachineAction` SHALL define action types WRENCH_CYCLE=0, SET_SIDE_CONFIG=1, CONFIG_UPDATED=2 with fields player_id, pos, face, new_role
- **AND** `MachineActionResp` SHALL carry success, error, pos, face, new_role, roles
- **AND** the client SHALL use `ToolAction` with `ToolActionType::WRENCH_CYCLE` (kToolAction=13) for side-config cycling, since `MachineAction` is not yet routed through Gateway

### Requirement: World Exploration
The system SHALL load chunks on demand from the client, generate them on miss, and cache them client-side.

#### Scenario: Client requests chunks on demand
- **GIVEN** the client camera moves into a new area
- **WHEN** `ChunkLoadManager::RunLoadPass` runs (rate-limited to 30 Hz)
- **THEN** it SHALL iterate the VIEW_RADIUS=8 × VERTICAL_RADIUS=3 cube around the camera chunk, frustum-cull candidates, and score them by distance/look-direction/velocity
- **AND** it SHALL send `PlayerAction{action=CHUNK_REQUEST, pos=Vec3i(cx,cy,cz)}` (kPlayerAction=1, ctrl port) for each chunk not already loaded or pending

#### Scenario: Gateway relays chunk requests
- **GIVEN** the gateway receives a `PlayerAction` from the client ctrl connection
- **WHEN** the action type is CHUNK_REQUEST
- **THEN** gateway SHALL publish it on topic `chunk.requests`
- **AND** SHALL drop MOVE/UNLOAD actions (client-driven loading; no server interest management)

#### Scenario: Chunk served from cache or LMDB
- **GIVEN** ChunkStore receives a chunk request on `chunk.requests`
- **WHEN** the chunk is in the lock-free CLOCK cache (1024 entries)
- **THEN** ChunkStore SHALL encode it to wire format and respond
- **AND** if not cached, SHALL read the chunk from LMDB (mmap, zero-copy)
- **AND** SHALL publish the encoded palette as `CompressedChunkData` (coord + palette_data) on topic `world.chunk.loaded.compressed`

#### Scenario: Chunk generated on cache miss
- **GIVEN** a requested chunk is neither in cache nor in LMDB
- **WHEN** `ChunkStore::AsyncGetChunk` calls `gen_queue_->requestChunk`
- **THEN** one of 8 generation worker threads SHALL run `WorldGenerator::GenerateTerrain` (2D Perlin terrain + 3D Simplex caves + ore veins)
- **AND** the result SHALL be encoded, cached, and written to LMDB in batches before the pending request callback fires

#### Scenario: Client receives and meshes chunk
- **GIVEN** gateway receives `CompressedChunkData` on `world.chunk.loaded.compressed`
- **WHEN** it forwards to the client on the bulk connection as `kCompressedChunkData` (12)
- **THEN** the client SHALL build a `ChunkView` from the palette data, store it in `ChunkStorage` (via `World::OnChunkData`), and rebuild the mesh on a TBB worker
- **AND** any block updates received before the chunk arrived SHALL be replayed over the fresh snapshot
- **AND** the mesh SHALL be uploaded to bgfx GPU buffers via `ProcessPendingOps`

#### Scenario: Client-side chunk eviction with TTL
- **GIVEN** the loaded chunk count exceeds MAX_CHUNKS=1024
- **WHEN** `ChunkLoadManager` selects the farthest chunks for eviction
- **THEN** they SHALL enter an eviction queue with a 5-second TTL
- **AND** re-access within the TTL SHALL cancel eviction
- **AND** after TTL expiry the chunk SHALL be evicted and an UNLOAD `PlayerAction` SHALL be sent (dropped by gateway — server never evicts)
