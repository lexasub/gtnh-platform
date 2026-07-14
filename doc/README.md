# Documentation Map

**GTNH Platform** — distributed Minecraft-style engine. This directory contains architecture docs,
design decisions, EPIC specifications, and operational notes.

## Quick Start

New to the project? Start here:

1. **[ROADMAP.md](../ROADMAP.md)** — project status: what's done, what's WIP, what's planned. Per-service breakdown, protocol status, data flows. **This is the single source of truth.**
2. **[C4 Diagrams](c4/README.md)** — visual architecture (Level 1–4): service topology, component internals, deployment, event flows. 55 PlantUML files.
3. **[init_adr.md](init_adr.md)** — architecture decisions made at inception. Why services are separate, why LMDB, why EnTT, etc.

## Reference

| File | Content | Status |
|------|---------|--------|
| [open_questions.md](open_questions.md) | Architectural Q&A — energy, fluids, ECS, chunk lifecycle | ✅ Current |
| [diff-protocol.md](diff-protocol.md) | Client↔Server block sync protocol. Partially implemented | 🟡 Partial |
| [performance_bug.md](performance_bug.md) | Head-of-line blocking in io_uring networking stack | 🟡 Not fully resolved |
| [init_adr.md](init_adr.md) | Initial Architecture Decision Record | ✅ Historical |
| [SPEC-gtnh-libs.md](SPEC-gtnh-libs.md) | Library spec: MachineRegistry, ECS components, protocol libs | 🟡 Active |
| [openspec specs](../openspec/) | Formal capability specs (OpenSpec format) | 🟡 Growing |

## EPIC Specifications

All EPICs have been completed and moved to [archive/EPICS/](archive/EPICS/).

## Planning Docs (Historical)

All planning docs have been completed or deferred. See [archive/](archive/) for historical reference.

## Operational

- [NEXT_STEPS.md](NEXT_STEPS.md) — active task tracking (P0/P1/P2)
- [war-stories.md](war-stories.md) — bugs and lessons learned
- [performance_bug.md](performance_bug.md) — io_uring HOL blocking deep-dive
- [optimization-audit-cpp26.md](optimization-audit-cpp26.md) — C++26 optimization opportunities

## Architecture Diagrams (C4)

See [c4/README.md](c4/README.md) for full index of 55 PlantUML diagrams covering:

| Level | Name | Coverage |
|-------|------|----------|
| L1 | System Context | Player, admin, external systems |
| L2 | Container | 13 services, TCP connections, MessageRouter pub/sub |
| L3 | Component | Per-service subsystems (SimCore, Client, ChunkStore, etc.) |
| L4 | Component Detail | Internal classes, components, data flows |

Generate PNGs: `plantuml doc/c4/level2-container.puml -tpng`

## Archived

| Directory | Content |
|-----------|---------|
| [archive/](archive/) | Initial architecture risk analyses (pre-implementation). Not current. |
