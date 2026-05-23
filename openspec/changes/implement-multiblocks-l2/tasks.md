## 1. SpatialIndex
- [ ] 1.1 R-tree implementation (bgi::rtree<AABB>)
- [ ] 1.2 Octree for entity queries
- [ ] 1.3 RPC: FindInRadius, FindAtPoint, FindEntitiesInAABB

## 2. Pattern Library
- [ ] 2.1 Generic pattern matching (replace ELECTROLYSER_PATTERN hardcode)
- [ ] 2.2 Pattern definitions for EBF (3x3x4), Large Boiler, LCR
- [ ] 2.3 Pattern validation on block change

## 3. Multiblock Systems
- [ ] 3.1 EBF tick system (heating coils, heat tiers, muffler)
- [ ] 3.2 Large Boiler tick (firebox, water→steam, multi-size)
- [ ] 3.3 LCR tick (fluid chemistry, catalysts)

## 4. Dissociation & Hatches
- [ ] 4.1 Dissociation detection (anchor block break → destroy multiblock)
- [ ] 4.2 Hatch detection and classification
- [ ] 4.3 sim.multiblock.created / destroyed topics

## 5. Persistence
- [ ] 5.1 Multiblock state save/load via EntityStateStore
