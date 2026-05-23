# Tasks: Multiblocks — Full Gameplay (L2)

## A. SpatialIndex Integration

- [ ] SpatialIndex сервис запущен (R-tree)
- [ ] Pattern registry в SimulationCore (EBF, Boiler, LCR)
- [ ] onBlockChanged → SpatialIndex query → pattern match
- [ ] Pattern match → MultiblockController entity

## B. Pattern Library (Generic)

- [ ] MultiblockPattern struct definition
- [ ] EBF pattern (3×3×4)
- [ ] Large Boiler pattern (3×3×4)
- [ ] LCR pattern (3×3×3)
- [ ] Hatch definitions (input/output/energy/fluid/item)
- [ ] Controller positions for each pattern
- [ ] Pattern registry in SimulationCore
- [ ] Generic matchPattern() function

## C. EBF (Electric Blast Furnace)

- [ ] EBF pattern in pattern library
- [ ] EBFSystem / multiblock tick handler
- [ ] Heating coil ID → max_heat mapping
- [ ] Recipe heat requirement check
- [ ] Input/Output/Energy hatch detection
- [ ] Muffler hatch (top)

## D. Large Steam Boiler

- [ ] Large Boiler pattern in pattern library
- [ ] Boiler tick system
- [ ] Fuel burning (coal, charcoal, fluid)
- [ ] Water input → steam conversion
- [ ] Overheat detection + damage
- [ ] Multi-size support (1×1×1 → 3×3×4)

## E. LCR (Large Chemical Reactor)

- [ ] LCR pattern in pattern library
- [ ] LCR tick system
- [ ] RecipeManager: fluid + solid input recipes
- [ ] Fluid hatch detection + management
- [ ] Byproduct handling

## F. Dissociation

- [ ] isMultiblockAnchor() — проверка mb_id
- [ ] Dissociation cascade: anchor → full cleanup
- [ ] mb_id clear from all pattern blocks
- [ ] Hatch contents ejection
- [ ] Client: multiblock visual removal

## Detailed Task Specifications

For detailed implementation specifications, see:
- [A-SpatialIndex-Integration.md](A-SpatialIndex-Integration.md) - SpatialIndex service implementation
- [B-Pattern-Library.md](B-Pattern-Library.md) - Generic pattern library
- [C-EBF.md](C-EBF.md) - Electric Blast Furnace implementation
- [D-Large-Boiler.md](D-Large-Boiler.md) - Large Steam Boiler implementation
- [E-LCR.md](E-LCR.md) - Large Chemical Reactor implementation
- [F-Dissociation.md](F-Dissociation.md) - Multiblock dissociation system
