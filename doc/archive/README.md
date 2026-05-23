# Archived Documents

These files were **initial architecture risk analyses** written in June 2026 (pre-implementation).
They contain detailed theoretical analysis of tradeoffs, risks, and design alternatives.

**Why archived**: The implementation diverged from several assumptions in these analyses.
Keeping them in the top-level `doc/` was misleading — they no longer reflect current architecture.

| File | Original Purpose | Why Archived |
|------|-----------------|--------------|
| `init_adr.analysis-router-protocol.md` | MessageRouter delivery guarantees, WAL, ordering | Router implementation is simpler (Go channels, no WAL). Analysis is academic. |
| `init_adr.analysis-storage.md` | EntityStateStore storage decisions | EntityStateStore went straight to LMDB (not in-memory). Analysis partially obsolete. |
| `init_adr.analysis-ecs-gateway.md` | Gateway/ECS architecture | ECS design decisions resolved differently in implementation. |
| `init_adr.analysis-recipe-sim.md` | RecipeManager/SimulationCore boundaries | RecipeManager embedded in SimulationCore (not separate service). Risks mostly resolved. |
| `init_adr.analysis-gaps.md` | Gap analysis of ADR decisions | Most gaps still open but irrelevant in current form. |
| `init_adr.analysis-techdebt.md` | Technical debt triggers and timeline | Multiple triggers already fired. Timeline obsolete. |
| `DESIGN_ENERGY_MACHINE_REGISTRY.md` | EnergyStorage, MachineRegistry, EnergyDistributionSystem design | EnergyDistributionSystem удалён. Энергораспределение делает PipeNetwork (см. ROADMAP.md). |

**Current architecture reference**: See `doc/init_adr.md` (updated with implementation changes),
`doc/ROADMAP.md`, and individual EPIC specs in `doc/EPICS/`.
