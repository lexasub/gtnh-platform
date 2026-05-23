# Next Steps

**Актуально на 2026-06-11** после BFS-анализа всех EPICS.

---

## Completed 2026-06-28

- **P0**: Autonomous Mining MVP — DrillSystem with spiral BFS, mining progress, output buffer, energy consumption
- **P1**: Heat Management — overheat detection (WARNING/CRITICAL), environment cooling, machine explosion, boiler heat/water→STEAM, MachineSystem 50% speed at WARNING
- **P2**: Chest Inventory Sync — ChestOpenReq/Resp protocol, gateway+netclient routing, chest window network sync
- **EPIC 8**: Questbook spec created (no implementation yet)
- **EPIC 9**: Autonomous Mining EPIC created
- **EPIC 4**: Electric Tools section status updated

---

## P0 — Immediate (continuation specs)

### Energy & Fluids — PipeNetwork integration

Энергия/жидкости: CreativeGenerator, PipeNetwork как отдельный сервис, жидкости как ItemStack.

| # | Задача | Статус |
|---|--------|--------|
| 1 | **CreativeGenerator configurable** — генератор с настраиваемой EU/tick для тестирования energy flow | ✅ DONE |
| 2 | **PipeNetwork как отдельный сервис** — main.cpp заглушка, не подключён к MessageRouter. BFS граф готов (`PipeNetworkManager`), distributeEnergy/distributeFluid написаны. Нужно: подписать на `world.blocks.changed`, вызывать solve на tick | ✅ DONE |
| 3 | **Fluid flow — тестовый source** — бесконечный источник жидкости для PipeNetwork (placeholder) | ✅ DONE |

**Spec:** `doc/EPICS/0-foundation-energy-fluids/0-foundation-energy-fluids.md`

---

### Item & Inventory — 2 remaining tasks

| # | Задача | Статус | Где |
|---|--------|--------|-----|
| 1 | **Server-authoritative grid state через EntityStateStore** — `WorkbenchStateManager` хранит грид в памяти, не подключён к EntityStateStore RPC. При открытии/закрытии верстака грид теряется | 🟡 WIP | `simulation_core/WorkbenchStateManager.*`, `entity_state_store.fbs` |
| 2 | **Inventory consumption через MetaDB** — `CraftRequestHandler::publishInventoryUpdate()` отправляет пустой `InventoryUpdate` без consumption delta. Реальный расход ингредиентов из инвентаря не публикуется | 🟡 WIP | `simulation_core/CraftRequestHandler.*`, `core.fbs` |

**Spec:** `doc/EPICS/0-foundation-item-inventory/0-foundation-item-inventory.md`

---

### Machines & Multiblocks — 3 remaining tasks

| # | Задача | Статус | Где |
|---|--------|--------|-----|
| 1 | **Серверная регистрация машин в мире** — при установке блока (`SetBlockAction` → SimulationCore) создавать ECS entity с `MachineComponent` | 🟡 WIP | `simulation_core/ECS/` |
| 2 | **Machine GUI** — client handler для `BlockEntityUpdate` (progress bar, energy, slots) | 🟡 WIP | `game_client/UI/` |
| 3 | **Multiblocks L2** — формирование/разрыв мультиблоков, сохранение через EntityStateStore | 🟡 WIP | `simulation_core/`, `entity_state_store/` |

**Spec:** `doc/EPICS/1-gameplay-machines-multiblocks/1-gameplay-machines-multiblocks.md`

---

## P1 — Core integration gaps

### Gateway interest management

`Gateway::ShouldSendChunk()` закомментирован — не фильтрует, какие чанки слать клиенту.

- Клиент должен слать `CHUNK_UNLOAD` action при выгрузке чанка
- Gateway хранит `subscribed_chunks{player_id → set<ChunkCoord>}`
- `PlayerActionType` enum: добавить `UNLOAD(5)`

**Файлы:** `gateway/public_server.cpp`, `gateway/main.cpp`, `ChunkLoadManager.cpp`, `core.fbs`

### WorldContainerInventory persistence

`WorldContainerInventory::storage_` = `nullptr` — персистентность не подключена. При перезапуске сервера содержимое игровых контейнеров теряется.

- Wire `storage_` → EntityStateStore RPC
- См. item-inventory continuation (P0 выше)

### Drag-and-drop state machine в SlotGrid

Клик-ту-пик/плейс. Нужен drag-with-mouse: dragItem следует за курсором, drop-on-empty/merge/swap.

**Файлы:** `game_client/UI/SlotGrid/SlotGrid.h`, `WorkbenchWindow.cpp`

### Macerator recipes

`macerator.json` отсутствует. Остальные 6 типов машин имеют рецепты.

**Файлы:** `data/recipes/`

---

## P2 — Polish & deferred

| Задача | Spec | Статус |
|--------|------|--------|
| Ores & processing (добыча, сортировка) | `3-infrastructure-automation` | Deferred — continuation spec |
| SpatialIndex (R-tree/Octree) | L2 spec | Не начат (`main.cpp` = 51 байт) |
| Single-thread tick performance | — | Deferred до 100+ машин |
| ImGui sync frequency | — | Deferred до 100+ машин |
| Inventory Actions (protocol breaking) | — | Post-MVP |
| NBT в ItemStack | — | Post-MVP |
| Chunk unload coordination | `3-infrastructure-automation` | Deferred |
| Chunk versioning | — | Low priority |
| Entity system (mobs, игроки) | — | Future |
| LZ4 compression | — | Future |

---

## ✅ Resolved (this session)

| Задача | Статус |
|--------|--------|
| ConditionEvaluator → реальный MachineState из ECS | ✅ DONE (`RecipeManager.cpp` overload) |
| CraftResponse UI feedback (цветной текст + таймер) | ✅ DONE |
| BlockEntityUpdate FlatBuffers таблица (hatches, covers, fluids) | ✅ DONE (`core.fbs`, `gateway.fbs` type 10) |
| FluidTank в протоколе — оставлен для UI | ✅ Done |
| RecipeManager рефакторинг → `src/libs/recipe_manager_lib/` | ✅ DONE (9 файлов) |
| MetaDB login/logout end-to-end | ✅ DONE (Gateway → `player.joined`/`player.left`) |
| 13 open questions — 10 решены, 2 deferred, 1 post-MVP | ✅ Done |
| 3 stale root files удалены (`GROOMING.md`, `task.md`, `tasks.md`) | ✅ Done |
| Archives: basic-mechanics, energy-fluids, recipe-system, crafting, shared-recipe-lib, item-inventory, machines-multiblocks, automation | ✅ Done |

---

## Legend

| Mark | Значение |
|------|----------|
| P0 | Блокирует весь прогресс — делать в первую очередь |
| P1 | Важно, но не блокирует P0 |
| P2 | Когда дойдут руки / defer |
| ✅ | Выполнено |
