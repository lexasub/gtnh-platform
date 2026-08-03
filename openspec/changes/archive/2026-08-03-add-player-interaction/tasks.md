## 1. CAS Block Placement
- [x] 1.1 Document SetBlockAction/SetBlockCASReq protocol (expected_block_id, new_block_id, conflict resolution)
- [x] 1.2 Verify optimistic ack flow (Client → Gateway → BlockAck(ACCEPTED) → Client, before CAS RPC completes)
- [x] 1.3 Verify conflict revert flow (CAS mismatch → BlockAck(CONFLICT, actual_id) → client revert via OnBlockUpdate)

## 2. Crafting Pipeline
- [x] 2.1 Document CraftRequest → RecipeManager (findRecipeByInputs) → CraftResponse flow
- [x] 2.2 Verify recipe types work end-to-end (macerator, furnace, compressor, alloy_smelter, extractor, mixer + crafting_table, assembler, electrolyser, chemical_reactor, crystallizer, generator, boiler)
- [x] 2.3 Document inventory deduction on craft success (setSlots + giveItem, quest detection hook)

## 3. Inventory System
- [x] 3.1 Document drag-and-drop state machine (DragManager: Idle/Holding, pickup/merge/swap/quick-move/drop/cancel)
- [x] 3.2 Verify MetaDB inventory persistence (per-mutation save via meta_db.inventory.set; load on login via player.inventory.load)
- [x] 3.3 Document InventoryAction protocol (MOVE=0, SPLIT=1, DROP=2; QUICK_MOVE=3 client-side)

## 4. Machine Interaction
- [x] 4.1 Document MachineWindow data-driven UI (progress styles by machine_class, energy bar, input/output slots, out-of-sync warning)
- [x] 4.2 Document machine state flow — push-based via BlockEntityUpdate (kBlockEntityUpdate=8, topic world.block_entity.update); NO QueryMachineState request exists
- [x] 4.3 Document MachineAction protocol (WRENCH_CYCLE/SET_SIDE_CONFIG/CONFIG_UPDATED — defined, not yet routed through Gateway; side-config uses ToolAction WRENCH_CYCLE)

## 5. World Exploration
- [x] 5.1 Document chunk request/load flow (CHUNK_REQUEST PlayerAction → chunk.requests → ChunkStore cache/LMDB → WorldGenerator on miss → world.chunk.loaded.compressed → client bulk)
- [x] 5.2 Verify FlatBuffers chunk serialization (CompressedChunkData palette_data; ChunkData blocks/meta/multiblock arrays)
- [x] 5.3 Document chunk caching on client side (ChunkLoadManager 1024-chunk cap, 5s TTL soft eviction, priority scoring, pendingRequests_ dedup)
