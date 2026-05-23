# Features / Эпики

> **Декомпозировано на эпики.** Каждая фича раскрыта в отдельном файле в `doc/EPICS/`.
> Архив выполненных эпиков: `doc/EPICS/archive/`.

---

## Layer 0: Фундаментальные примитивы

| Эпик | Файл | Статус |
|------|------|--------|
| **Item & Inventory** — ItemStack, инвентари, EntityStateStore | [`0-foundation-item-inventory.md`](EPICS/0-foundation-item-inventory/0-foundation-item-inventory.md) | 🟡 Continuation (2 tasks) |
| **Recipe System** — рецепты, RecipeManager, условия | [`0-foundation-recipe-system`](EPICS/archive/0-foundation-recipe-system/) | ✅ Done (refactored to `src/libs/recipe_manager_lib/`) |
| **Energy & Fluids** — EU, EnergyStorage, жидкости, PipeNetwork | [`0-foundation-energy-fluids.md`](EPICS/0-foundation-energy-fluids/0-foundation-energy-fluids.md) | 🔴 Continuation (CreativeGenerator, PipeNetwork) |
| **Luanti Patterns** (external reference) | — | ↪ Пропущен (не наш код) |

## Layer 1: Базовые механики

| Эпик | Файл | Статус |
|------|------|--------|
| **Basic Mechanics** — мир, блоки, чанки, взаимодействие | [`0-basic-mechanics`](EPICS/archive/0-basic-mechanics/) | ✅ Done |
| **Player & Crafting** — инвентарь игрока, верстак, крафт | [`1-gameplay-player-crafting`](EPICS/archive/1-gameplay-player-crafting/) | ✅ Done |
| **Machines & Multiblocks** — ECS tick, мультиблоки, GUI | [`1-gameplay-machines-multiblocks.md`](EPICS/1-gameplay-machines-multiblocks/1-gameplay-machines-multiblocks.md) | 🟡 Continuation (3 tasks: world→ECS, GUI, L2) |

## Layer 2 (отложено): Мультиблоки, электричество, пространственные запросы

> SpatialIndex (R-tree/Octree) — deferred до L2. Сейчас multiblock queries прямо в SimulationCore.
> Energy flow — PipeNetwork как отдельный сервис.
> Спеки: `foundation-energy-fluids.md`, `gameplay-machines-multiblocks.md`.

## Layer 3 (отложено): Автоматизация и логистика

| Эпик | Файл | Статус |
|------|------|--------|
| **Infrastructure & Automation** — чанк анлоад, ores, автоматизация | [`3-infrastructure-automation.md`](EPICS/3-infrastructure-automation/3-infrastructure-automation.md) | 🔴 Continuation (ores deferred) |

---

## Ключевые правила

- **Энергия** — просто число, без физики, провода как декорация/топология
- **GUI** — ImGui, без кастомного рендера
- **Нет дропа** — сломал блок → сразу в инвентарь
- **Состояние машин** — EntityStateStore (отдельный C++ сервис, LMDB, TCP :5200)
- **Без античита** — полное доверие клиенту
- **RecipeManager** — вынесен в `src/libs/recipe_manager_lib/` (shared library, не отдельный RPC-сервис)
- **Крафт** — только через блок верстака (shape-aware 3×3, io_uring)
- **Инструменты / руды / редстоун** — не делаем
- **Fluids** — как ItemStack, не отдельный тип (нет FluidTankComponent)
- **PipeNetwork** — отдельный C++ сервис (main.cpp заглушка, BFS-граф готов)

---

## Текущее состояние реализации

| Компонент | Статус | Детали |
|-----------|--------|--------|
| **MessageRouter** | ✅ | Go, pub/sub, 100k topics, heartbeat |
| **Gateway** | ✅ | TCP :3000, interest mgmt (ShouldSendChunk закомментирован), io_uring |
| **ChunkStore** | ✅ | LMDB, чанки 32³, SetBlock/GetBlock, meta-layer |
| **WorldGenerator** | 🟡 | FastNoiseLite, flat world (stub — нет руд) |
| **SimulationCore** (ECS) | 🟡 | EnTT, MachineSystem 20Hz tick, multiblock detection, RecipeManager eval. Machine→World регистрация не сделана. |
| **EntityStateStore** | ✅ | C++ LMDB-сервис, TCP RPC :5200, pub/sub `entity.state.get/set` |
| **PipeNetwork** | 🔴 | main.cpp заглушка (50 байт). BFS-граф, distributeEnergy/distributeFluid написаны. Не подключён к MessageRouter. |
| **SpatialIndex** | 🔴 | main.cpp заглушка (51 байт). L2 deferred. |
| **MetaDB** | ✅ | Go/SQLite, подключён к MessageRouter. Player joined/left end-to-end. Inventory chain не завершена. |
| **GameClient** | 🟡 | bgfx, ImGui, FPS cam, DDA raycast, workbench 3×3 grid, BlockEntityUpdate handler. Drag-and-drop не реализован. |
| **RecipeManager** | ✅ | `src/libs/recipe_manager_lib/` — 9 файлов. JSON-рецепты (6 типов). ConditionEvaluator с MachineState из ECS. |
| **Craft Pipeline** | 🟡 | CraftRequest (type 9) / CraftResponse (type 10). Workbench grid pattern-matching. Inventory consumption не подключён. |

## FlatBuffers протокол (type_id 1-10)

| Type | Название | Направление | Статус |
|------|----------|-------------|--------|
| 1 | PlayerAction | client→gw | ✅ |
| 2 | ChunkSnapshot | gw→client | ✅ |
| 3 | EntitySnapshot | gw→client | ✅ |
| 4 | BlockUpdate | gw→client | ✅ |
| 5 | MultiblockCreatedEvent | gw→client | ✅ |
| 6 | InventoryUpdate | gw→client | ✅ |
| 7 | InventoryAction | client→gw | ✅ |
| 8 | ChunkRequest | client→gw | ✅ |
| 9 | CraftRequest | client→gw | ✅ |
| 10 | BlockEntityUpdate | gw→client | ✅ (hatches, covers, fluids, mb_id, structure) |

## Active continuation specs (что делать сейчас)

1. **Energy & Fluids** — CreativeGenerator, PipeNetwork интеграция, fluid source
2. **Item & Inventory** — EntityStateStore grid persistence, MetaDB consumption
3. **Machines & Multiblocks** — World→ECS registration, Machine GUI, L2 multiblocks

Подробнее: [`doc/NEXT_STEPS.md`](NEXT_STEPS.md)

---

*Обновлено: июнь 2026 — после BFS-анализа всех EPIC, 13 open questions resolved, архивация завершённых эпиков.*
