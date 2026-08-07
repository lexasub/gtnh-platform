# ROADMAP

**GTNH Platform** — распределённый Minecraft-style движок. C++ performance core, Go sidecars.
FlatBuffers + TCP. MessageRouter (Go) — внутренний pub/sub.

## Условные обозначения

- ✅ DONE — работает и используется
- 🟡 WIP — в процессе / частично реализовано
- 🔴 TODO — не начато
- ⏸ DEFERRED — отложено (L2+)

---

# Этап 0: MVP Core ✅

**Сервисы собираются, стартуют, общаются через MessageRouter.**

## Сервисы

| Компонент | Язык | Статус | Линк | Что делает |
|-----------|------|--------|------|------------|
| MessageRouter | Go | ✅ | `:4000` | Pub/sub: `player.actions`, `world.chunk.loaded`, `world.blocks.changed`. 3 уровня приоритета, heartbeat, service discovery |
| Gateway | C++ | ✅ | `:7777` (ctrl), `:7778` (bulk), `:4000` (router) | TCP сервер, interest mgmt, форвардинг, io_uring (libgtnh-net) |
| ChunkStore | C++ | ✅ | `:5001` (RPC), `:4000` (router) | LMDB, чанки 32³, SetBlock/GetBlock, meta-layer для mb_id. Переписан на libgtnh-net (io_uring, 2026-07-31), palette-native MutableChunk |
| WorldGenerator | C++ | ✅ | **Библиотека, не сервис** (линкуется в chunkd) | FastNoiseLite, OreGenerator (GTNH-вейны), TreeGenerator, SurfaceHeights, GenerationQueue |
| SimulationCore | C++ | ✅ | `:4000` (router), RPC → `:5001`, `:5200` | ECS (EnTT), MachineSystem 20Hz tick, мультиблоки L2+L3, heat, quests, DrillSystem, hatches |
| PipeNetwork | C++ | ✅ | `:4000` (router) | BFS граф (CableGraph, PipeNetworkManager), energy/fluid/item, per-tick demand, HeatLoss, трансформаторы, перегрев, взрывы |
| SpatialIndex | C++ | 🔴 | `:—` | **СТАБ** — `main.cpp` = 2 строки, `add_subdirectory` закомментирован, не собирается. R-tree/Octree planned |
| EntityStateStore | C++ | ✅ | `:5200` (RPC), `:4000` (router) | LMDB-backed entity persistence, pub/sub `entity.state.get/set` |
| MetaDB | Go | ✅ | `:5005` (JSON API), `:5006` (FlatBuffers RPC) | SQLite, квесты, награды, exchange, инвентари, player joined/left end-to-end |
| GameClient | C++ | ✅ | `:7777` (gw ctrl), `:7778` (gw bulk) | bgfx, GLFW, ImGui, survival physics, game modes, quest UI, hotbar, NEI |
| **RecipeManager** | C++ | ✅ | `:5555` (router RPC, аргумент `--router-port`) | **Новый standalone сервис** — recipe check/craft/catalog queries. Бинарь `reciped` |
| RecipeManagerLib | C++ | ✅ | `src/libs/recipe_manager_lib/` | RecipeManager.cpp (1065 строк), ItemRegistry, ConditionEvaluator с MachineState из ECS |
| StorageInterfaces | C++ | ✅ | `src/services/storage_interfaces/` | Заголовки IEntityStateStorage.h, IPlayerInventoryStorage.h — не сервис |
| Validation | C++ | 🔴 | `:—` | Item/block validation. **НЕ в корневом CMakeLists** — не собирается по умолчанию |

Примечание: **ChestSync** — протокольная фича (ChestOpenReq/Resp) + клиентское окно, не сервис.
**DrillSystem** — ECS-система внутри SimulationCore, не сервис.

## FlatBuffers схемы (12 файлов, `src/protocol/`)

| Файл | Статус | Сообщения |
|------|--------|-----------|
| `protocol/core.fbs` | ✅ | Vec3i, Vec3f, ItemStack, PlayerAction, ChunkData, EntitySnapshot, BlockChangedEvent, BlockEntityUpdate, MultiblockCreatedEvent, InventoryUpdate |
| `protocol/gateway.fbs` | ⚠️ | GatewayPayload union — **УСТАРЕЛ**, реальный провод = C++ константы `GatewayMsg` (см. ниже) |
| `protocol/chunkstore.fbs` | ✅ | GetBlock/SetBlock/GetChunk/SaveChunk RPC |
| `protocol/simcore.fbs` | ✅ | BlockChangedReq, MatchPatternReq, TickReq + resp |
| `protocol/recipe.fbs` | ✅ | CheckRecipeReq, CraftReq, EvaluateConditionsReq, MachineType enum (NONE..CHEMICAL_REACTOR) |
| `protocol/entity_state_store.fbs` | ✅ | GetEntityStateReq/Resp, SetEntityStateReq/Ack RPC |
| `protocol/tile_entity_store.fbs` | ✅ | TileEntity save/load RPC |
| `protocol/machine_state.fbs` | ✅ | MachineState RPC (NEW) |
| `protocol/multiblock_state.fbs` | ✅ | MultiblockState blob (NEW) |
| `protocol/meta_db.fbs` | ✅ | Player inventory/position/state RPC |
| `protocol/pipe_network.fbs` | ✅ | ItemNodeUpdate, ItemTransferReq/Resp, ItemFlowEvent, fluid/energy протокол |
| `protocol/quest.fbs` | ✅ | Quest book protocol (NEW) |

`item_registry.fbs` **НЕ СУЩЕСТВУЕТ** — item registry = `data/registry/items.csv` + items.db.
Мёртвые члены union'а (в .fbs, нет C++-использования): GridUpdate, MachineAction, MachineActionResp — кандидаты на чистку.

## Протокол клиент-гейтвей

Формат фрейма: `[4B size BE][1B msg_type][FlatBuffer]`.

**Авторитетный источник — C++ константы `GatewayMsg` (1-based, 41 тип)** в `gateway.h`/`NetClient.h`:

```
 1 = PlayerAction           22 = QuestCompletedNotification
 2 = ChunkSnapshot          23 = MultiblockEvent
 3 = EntitySnapshot         24 = QuestCompleteRequest
 4 = BlockUpdate            25 = QuestEraTransition
 5 = BlockAck               26 = QuestExchangeRequest
 6 = InventoryUpdate        27 = QuestExchangeResponse
 7 = InventoryAction        28 = QuestExchangeCooldownGet
 8 = BlockEntityUpdate      29 = QuestExchangeCooldown
 9 = CraftRequest           30 = GameModeChange
10 = CraftResponse          31 = StartScenarioReq
11 = SetBlockAction         32 = StartScenarioResp
12 = CompressedChunkData    33 = QuestBookOpen
13 = ToolAction             34 = RecipeCheckReq
14 = ToolActionResp         35 = RecipeCheckResp
15 = SetMachineSlot         36 = RecipeCatalogReq
16 = SetMachineSlotResp     37 = RecipeCatalogResp
17 = RecipeCompleted        38 = RecipeItemReq
18 = ChestOpenReq           39 = RecipeItemResp
19 = ChestOpenResp          40 = RecipeMachineReq
20 = QuestProgressUpdate    41 = RecipeMachineResp
21 = QuestUnlockNotification
```

## Startup log

```
[info] ChunkStoreClient connected to 127.0.0.1:5001
[info] SimulationCore connected to router on 127.0.0.1:4000
[info] SimulationCore started
[info] RouterClient connected to 127.0.0.1:4000
[info] MetaDB connected to router
[info] EntityStateStore listening on :5200
```

---

# Этап 1: Стабилизация геймплея ✅

**Цель:** клиент подключается, видит мир, ходит, ломает и ставит блоки.

## Починено

- [x] Crosshair — `RenderThread.cpp` ImGui `GetForegroundDrawList()`
- [x] Block propagation — инкрементальный `BlockUpdate` вместо full chunk
- [x] Topic names — simulation_core подписан на `player.actions` (мн.ч.)
- [x] Mesh eviction leak — `World::EvictChunk` → `pendingEvicted_`
- [x] Movement threshold — ChunkLoadManager с 2→1 чанк
- [x] Negative coords — `ChunkStore::makeKey` bias-кодировка

## Data flow (работает)

```
Client LMB
  ↓
Gateway :7777 → Router `player.actions`
  ↓
SimulationCore → RPC SetBlockReq → ChunkStore
  ↓
ChunkStore.SetBlockAsync → callback → publish `world.blocks.changed`
  ↓
Router → Gateway → Client :7777 (msg_type=4, FlatBuffer=BlockChangedEvent)
  ↓
NetClient::ProcessBlockUpdate → World::OnBlockUpdate → ChunkView::SetBlock → mesh rebuild
```

## Осталось

### 1.1 Gateway interest management
- **Проблема:** Gateway шлёт чанки, которые клиент уже выгрузил
- Клиент шлёт `CHUNK_UNLOAD` action при выгрузке чанка
- Gateway хранит `subscribed_chunks{player_id → set<ChunkCoord>}`
- **Файлы:** `public_server.cpp`, `main.cpp`, `ChunkLoadManager.cpp`
- **Протокол:** добавить `UNLOAD(5)` в `PlayerActionType` (`core.fbs`)

### 1.2 WorldContainerInventory persistence
- `WorldContainerInventory::storage_` — персистентность через EntityStateStore RPC
- См. item-inventory continuation spec

### 1.3 Chunk versioning (опционально)
- ChunkStore хранит `version:uint64`, инкрементит при `SetBlock`
- **Приоритет:** низкий (нет сетевых race)

---

# Этап 2: Мультиблоки ✅ (L2+L3)

**Цель:** поставить 3×3×3 специальных блоков → SimulationCore находит паттерн → создаёт MultiblockController.

## Реализовано

- [x] **Pattern registry** — паттерны мультиблоков, не только Electrolyser
- [x] **EBF / Boiler / LCR** — системы мультиблоков (08-02)
- [x] **SetBlockMeta RPC** — SimulationCore пишет `mb_id` в meta-layer ChunkStore
- [x] **ECS компоненты** — `MachineComponent`, `RecipeProgress`, `InventoryContainer`, `EnergyStorage`
- [x] **MachineSystem** — 20Hz tick, recipe matching, energy consumption, progress
- [x] **Hatches** — детекция хатчей + аллокация слотов (08-03)
- [x] **Item IO** — ввод/вывод предметов через хатчи
- [x] **Block-break guard** — защита от разрушения структурных блоков
- [x] **Persistence** — сохранение мультиблоков (SimulationCore ↔ EntityStateStore)
- [x] **Client GUI** — MachineWindow: progress bars, energy/heat/steam, overheat, hatches
- [x] **FlowHandlers** — обработка потоков мультиблоков (08-03)
- [x] **BlockEntityUpdate протокол** — FlatBuffers таблица (hatches, covers, fluids, mb_id, structure_valid, network_id), тип 8

**Spec:** `openspec/specs/implement-multiblocks-l2/`, `openspec/specs/multiblocks-l3/`

---

# Этап 3: PipeNetwork ✅

**Цель:** трубы проводят энергию/жидкость/предметы. Строишь трубы → PipeNetwork решает граф → flow_map.

## Реализовано

- [x] **BFS граф** — `PipeNetworkManager` в `PipeNetwork.h/.cpp`
- [x] **distributeEnergy / distributeFluid** — базовые алгоритмы распределения
- [x] **Per-tick energy demand** + loss calcs + network health (07-08)
- [x] **MessageRouter integration** — подписка `energy.node.update`, `energy.consume.request`, `energy.check.request`; публикация `energy.consume.response`, `energy.flow`
- [x] **CableGraph** — пакетная маршрутизация электричества (BFS, voltage/ampacity, overheat)
- [x] **Item Pipe Network** — перемещение предметов через BFS от source к sink, item buffering (08-03)
- [x] **Fluid Protocol + Service** — `fluid.node.update/check.request/consume.request`, FluidRegistry (water=84, steam=85, sulfuric_acid=86)
- [x] **HeatLoss module** — теплоотвод труб (08-03)
- [x] **Tiered cables** — примыкание кабелей по тиру (08-03)
- [x] **Transformer ECS** — `TransformerComponent` + `TransformerSystem` (MV/HV step-up/down)
- [x] **PipeMeshBuilder** — корректные ID блоков (ITEM_PIPE→62, FLUID_PIPE→61, кабели 66-71), UV-текстуры
- [x] **Перегрев и взрывы** — Cable overheat detection (WARNING/CRITICAL), explosion

## Осталось

- [ ] Multi-dimension (2 инстанса)

---

# Этап 4: SpatialIndex 🔴 (L2, deferred)

**Цель:** быстрые пространственные запросы — R-tree для мультиблоков, Octree для entity.

**Статус:** `src/services/spatial_index/main.cpp` = 2 строки (`int main(){return 0;}`),
`add_subdirectory` закомментирован в корневом CMakeLists.txt — сервис **не собирается**.

- R-tree (`bgi::rtree<AABB>`) для bounding box мультиблоков
- Dynamic octree для entity queries
- RPC: FindInRadius, FindAtPoint, FindEntitiesInAABB

**Причина defer:** текущие multiblock queries работают напрямую в SimulationCore.
SpatialIndex понадобится при 100+ мультиблоков в одном чанке.

---

# Этап 5: GameClient улучшения 🟡

**Цель:** клиент перестаёт быть tech-demo.

| Задача | Статус |
|--------|--------|
| Block atlas (UV-координаты) | ✅ TextureAtlas, FaceTextureRegistry, ChunkMeshBuilder UVs |
| Hotbar + block picking | ✅ SlotGrid::RenderHotbar, ActionHandler DoSelectHotbar/DoScrollHotbar, клавиши 1-9,0 |
| Drag-and-drop в инвентаре | ✅ DragManager (285 строк), 14 юнит-тестов: pickup/drop/merge/swap/split/shift-click/ESC/Q |
| Machine windows | ✅ MachineWindow (635 строк, data-driven), BlockUIFactory, ChestWindow |
| NEI panel | ✅ NeiPanel (232 строки), toggle `U`, клик-спавн |
| Quest book UI | ✅ QuestBookWindow (497 строк), toggle `~` |
| Crafting UI | ✅ CraftingWindow, CraftingGrid 3×3, server-driven preview, toast |
| Sound (miniaudio) | 🟡 miniaudio подключён в CMake, аудио-кода нет |
| Drill UI | 🟡 только тултип (energy/progress в SlotGrid), отдельного окна нет |
| Pause menu, settings | 🔴 отсутствует |

---

# Этап 6: Инфраструктура 🟡

- [x] CI (GitHub Actions, `.github/workflows/build.yml`) — anti-drift guards (07-14), pre-commit always_run, gate parity (08-05)
- [x] `--version` флаг + SIGUSR1 metrics во все сервисы (07-20)
- [x] Dependency management (Conan, toolchain в cmake-build-*)
- [ ] Packaging (AppImage / Docker compose)
- [ ] Graceful shutdown, health checks (частично)

---

# Этап 7: Будущее ⏸

- Entity system (мобы, игроки в ECS)
- Networking v2 (LZ4, rate limiting, reconnection, channels) — request_id-трейсинг уже есть (07-19/07-30)
- Mod runtime (C++ `.so`/`.dlopen`, Lua/Python deferred)
- AssetServer
- Scale (HTTP/3 + QUIC, шардирование, web client)

---

# Этап 8: Crafting Pipeline ✅

**Цель:** крафт через RecipeManager.

## Реализовано

- [x] **GridPatternMatcher** — shape-aware 3×3 matching с rotation/reflection (8 трансформаций)
- [x] **YAML миграция** — рецепты из JSON → YAML (коммит c5cfc39, 2026-06-28), JSON-парсинг удалён (eb599b5)
- [x] **14 YAML-рецептов** — assembler, boiler, bronze_alloy_smelter, chemical_reactor, compressor, crafting_table, crystallizer, ebf, electrolyser, extractor, furnace, generator, **macerator** (есть!), mixer
- [x] **energy_type matching** — рецепты по типу энергии
- [x] **Hierarchical packed item IDs** — unified GTNH-2qi (08-03), 3×3 positional matching (da3b5a2)
- [x] **CraftRequestHandler** — protocol types 9/10, shape-aware matching
- [x] **ConditionEvaluator** — MachineState из ECS (`RecipeManager.cpp` overload). Больше не пустой placeholder
- [x] **CraftResponse UI feedback** — цветной текст + таймер в CraftingWindow
- [x] **RecipeManager → standalone сервис** — `src/services/recipe_manager/` (`reciped`, RPC :5555), lib `recipe_manager_lib/`

## Осталось

- [ ] **Server-authoritative grid state** — server grid через TileEntityStore RPC
- [ ] **Inventory consumption delta** — `publishInventoryUpdate()` шлёт пустой `InventoryUpdate` без дельты

---

# Этап 9: Инвентарная система ✅

## Реализовано

- [x] **NetClient handlers** — InventoryUpdate (тип 6), InventoryAction (тип 7)
- [x] **Gateway forwarding** — pub/sub `player.inventory.update` → MetaDB, `player.inventory.action` → SimulationCore
- [x] **MetaDB pub/sub** — `PublishInventoryUpdate()`, `PublishInventoryAction()`
- [x] **EntityStateStore RPC** — GetEntityStateReq/Resp, SetEntityStateReq/Ack (LMDB, :5200)
- [x] **Player joined/left** — end-to-end (Gateway `player.joined`/`player.left` → MetaDB)
- [x] **Drag-and-drop** — DragManager state machine (Idle→Holding), 14 тестов, machine drag context
- [x] **PlayerInventory UI** — окно (E key), 27 слотов

## Осталось

- [ ] Выгрузка crafted items в player inventory
- [ ] Синхронизация с MetaDB при коннекте

---

# Этап 10: Quests & Game Modes ✅

## Quests (квестовая система, конец-2026-07 … 08-06)

- [x] **Базовый quest system** (07-03) — quest_lib data model + quest graph
- [x] **Server-side detection** — дефиниции квестов в MetaDB (CSV), topic split (08-03)
- [x] **Wire протокол** — FlatBuffers QuestProgressUpdate (20), QuestUnlockNotification (21), QuestCompletedNotification (22), QuestBookOpen (33)
- [x] **Награды** — quest rewards → инвентарь + энергия бура через ItemEnergyStorage (08-03)
- [x] **Manual completion** — server-authoritative (08-03)
- [x] **Era transition** + detection handlers + exchange market (08-05)
- [x] **INVENTORY-type детекция** + QuestBookOpen (08-06)
- [x] **QuestBookWindow** — era tabs, INVENTORY/EXCHANGE детекция, cooldown, complete/exchange кнопки

## Game Modes (08-06)

- [x] **Console + `/gamemode`** — режимы SURVIVAL/CREATIVE/ADVENTURE/SPECTATOR
- [x] **Mode sync** — gateway ↔ client, flight toggled by mode
- [x] **Game scenario** — `/startGameScenario` (StartScenarioReq/Resp)

## Survival Physics (08-06)

- [x] **Гравитация/прыжок/sneak** — Camera.cpp (gravity 25.0, jump 8.5, sneak eye-height + edge-stop)
- [x] **Коллизии** — per-axis AABB, ground ray scan

---

# Архитектурные заметки

## Work tracking

Active work is tracked as **openspec changes** (`openspec/changes/<id>/`). Each change has a `proposal.md` (why/what), `tasks.md` (checklist), and formal spec deltas.

```
openspec/changes/
├── add-game-modes/                  # 🟡 WIP — game modes + console
├── add-quest-exchange/              # ✅ done — exchange market
├── add-quest-inventory-detection/   # ✅ done — INVENTORY-type detection
├── add-texture-system/              # 🟡 WIP
├── add-tree-generation/             # 🟡 WIP — TreeGenerator + SurfaceHeights
├── complete-autonomous-mining/      # 🟡 WIP — drill persistence, UI
├── fix-ore-processing-chain/        # 🟡 WIP
├── implement-multiplayer-sync/      # 🟡 WIP
├── init-game-flow/                  # ✅ done
├── questbook-client-polish/         # 🟡 WIP
└── questbook-detection-handlers/    # ✅ done
```

Архив: `openspec/changes/archive/` (electric-tools-wrench, multiblocks-l2, questbook,
pipes-cables-transport, explosion-mechanics, unify-recipe-ids).
Live specs: `openspec/specs/`.

## Где что лежит

| Что нужно сделать | Куда идти |
|------------------|-----------|
| Добавить FlatBuffers сообщение | `src/protocol/*.fbs` (12 файлов) |
| Изменить протокол клиент-гейтвей | `gateway/gateway.h`, `gateway/main.cpp`, `game_client/Network/NetClient.*` |
| Player save/load, квесты | `meta_db/main.go` |
| Gateway форвардинг | `gateway/message_router_client.*`, `gateway/public_server.*` |
| Логика чанков | `chunk_store/`, `game_client/World/`, `game_client/Cache/` |
| Симуляция / ECS / мультиблоки | `simulation_core/` |
| Energy / liquids / items | `pipe_network/` |
| Spatial queries | `spatial_index/` (стаб, не собирается) |
| Client UI / Render | `game_client/UI/`, `game_client/Render/` |
| Recipe system | `data/recipes/` (YAML), `libs/recipe_manager_lib/`, `services/recipe_manager/` |
| Entity persistence | `entity_state_store/` |
| Quest data model | `libs/quest_lib/`, `src/protocol/quest.fbs` |

## Сборка

**Никогда не пересобирать с нуля, не удалять `cmake-build-debug/` / `cmake-build-release/`** — внутри Conan toolchain.

```bash
cd cmake-build-debug
ninja -j5
```

Go-сервисы (routerd, metadbd) собираются через `go build` (см. run.sh).

## Запуск (порядок важен)

```bash
./cmake-build-debug/src/services/message_router/routerd            # 1. Pub/sub (Go, :4000)
./cmake-build-debug/src/services/chunk_store/chunkd                # 2. World (C++, :5001)
./cmake-build-debug/src/services/entity_state_store/entitystated   # 3. Entity state (C++, :5200)
./cmake-build-debug/src/services/gateway/gatewayd                  # 4. Gateway (C++, :7777/:7778)
./cmake-build-debug/src/services/simulation_core/simcored_exec     # 5. Simulation (C++, 20Hz)
./src/services/meta_db/metadbd                                     # 6. Player DB (Go, :5005/:5006)
./cmake-build-debug/src/services/pipe_network/pipenetworkd         # 7. PipeNetwork (C++)
./cmake-build-debug/bin/gameclientd                                # 8. Client (C++, bgfx)

# spatial-index — стаб, не собирается/не запускается
# recipe_manager (:5555) — standalone, запускать отдельно при необходимости
```

**Или одной командой:** `./run.sh` (сборка ninja + Go, запуск всего; `--all` добавит pipenetworkd/spatialindexd/validationd; `--no-client` без клиента).

## Известные проблемы

| Проблема | Где | Статус |
|----------|-----|--------|
| GatewayMsg C++ константы vs FlatBuffers `GatewayPayload` union — расхождение | `gateway.fbs` | 🔴 TODO |
| Мёртвые сообщения GridUpdate / MachineAction / MachineActionResp | `gateway.fbs` | 🔴 TODO |
| Pause menu / settings отсутствует | `game_client/` | 🔴 TODO |
| Sound: miniaudio подключён, аудио-кода нет | `game_client/` | 🟡 WIP |
| SpatialIndex не собирается (add_subdirectory закомментирован) | `CMakeLists.txt` | 🔴 TODO |
| Drill UI — только тултип | `game_client/UI/` | 🟡 WIP |
| Server-authoritative grid state | crafting | 🔴 TODO |
| Inventory sync с MetaDB при коннекте | `MetaDB/` | 🔴 TODO |

## Ключевые архитектурные решения

### Energy = число, fluids = ItemStack
- EnergyStorage — ECS-буфер. PipeNetwork считает поток.
- Fluids/gas/plasma — как `ItemStack`, не отдельный тип. Нет `FluidTankComponent`.

### RecipeManager — shared library + standalone сервис
- Lib `src/libs/recipe_manager_lib/` (RecipeManager.cpp 1065 строк) + сервис `src/services/recipe_manager/` (`reciped`, RPC :5555)

### BlockEntityUpdate — FlatBuffers table
- В `core.fbs` с HatchInfo, CoverInfo, FluidTank, mb_id, structure_valid, network_id. Тип 8 в протоколе

### MessageRouter: io_uring вместо NATS/Redis

| Фактор | io_uring | NATS/Redis |
|--------|----------|------------|
| **Латентность** | 0.1-0.5 ms | 1-3 ms |
| **Память** | ~500KB/коннект | ~3-5MB/процесс |
| **Syscalls** | 0-1 на msg | TCP + TLS + protobuf |

### Open questions (13 resolved, 10 → 2 deferred → 1 post-MVP)

| Q | Тема | Решение |
|---|------|---------|
| Q1 | Energy source | CreativeGenerator configurable |
| Q2 | Energy: ECS or PipeNetwork | PipeNetwork |
| Q3 | FluidSlots в MachineState | Fluids as items |
| Q4 | FluidTank в протоколе dead? | Оставить для UI |
| Q5 | PipeNetwork: service or lib | Отдельный сервис |
| Q6 | SpatialIndex на L1? | Нет (L2) |
| Q7 | MetaDB login/logout | End-to-end ✅ |
| Q8 | BlockEntityUpdate формат | FlatBuffers ✅ |
| Q9 | Inventory Actions breaking | Post-MVP |
| Q10 | Recipe auto-selection | UI как GTNH |
| Q11 | Fluid flow: соседи или граф | PipeNetwork |
| Q12 | Single-thread tick | Deferred (100+ machines) |
| Q13 | ImGui sync freq | Deferred (100+ machines) |

Детали: `doc/open_questions.md`

---

**Legend:** ✅ DONE | 🟡 WIP | 🔴 TODO | ⏸ DEFERRED
**Updated:** 2026-08-07 — docs refreshed to current state; WIP tracked as openspec changes.
