## 1. CAS Block Placement
- [ ] 1.1 Document SetBlockCAS protocol (expected_id, new_id, conflict resolution)
- [ ] 1.2 Verify optimistic ack flow (Client → Gateway → BlockAck → Client)
- [ ] 1.3 Verify conflict revert flow (CAS mismatch → CONFLICT → client revert)

## 2. Crafting Pipeline
- [ ] 2.1 Document CraftRequest → RecipeManager → CraftResponse flow
- [ ] 2.2 Verify 6 recipe types work end-to-end (macerator, furnace, compressor, alloy_smelter, extractor, mixer)
- [ ] 2.3 Document inventory deduction on craft success

## 3. Inventory System
- [ ] 3.1 Document drag-and-drop state machine (DragManager)
- [ ] 3.2 Verify MetaDB inventory persistence (save/load on logout/login)
- [ ] 3.3 Document InventoryAction protocol (MOVE, SWAP, DROP)

## 4. Machine Interaction
- [ ] 4.1 Document MachineWindow data-driven UI (progress bars, energy, slots)
- [ ] 4.2 Document QueryMachineState → MachineState flow
- [ ] 4.3 Document MachineAction (SET_INPUT, SET_MODE) protocol

## 5. World Exploration
- [ ] 5.1 Document chunk request/load flow (Client → Gateway → ChunkStore → WorldGenerator)
- [ ] 5.2 Verify FlatBuffers chunk serialization
- [ ] 5.3 Document chunk caching on client side
