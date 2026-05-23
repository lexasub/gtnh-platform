# ROADMAP

**GTNH Platform** — распределённый Minecraft-style движок. C++ performance core, Go sidecars.
FlatBuffers + Asio TCP. MessageRouter (Go) — внутренний pub/sub.

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
| MessageRouter | Go | ✅ | `:4000` | Pub/sub: `player.actions`, `world.chunk.loaded`, `world.blocks.changed` |
| Gateway | C++ | ✅ | `:7777` (ctrl), `:7778` (bulk), `:4000` (router) | TCP сервер, interest mgmt (ShouldSendChunk закомментирован), форвардинг, io_uring |
| ChunkStore | C++ | ✅ | `:5001` (RPC), `:4000` (router) | LMDB, чанки 32³, SetBlock/GetBlock, meta-layer для mb_id |
| WorldGenerator | C++ | 🟡 | **Библиотека, не сервис.** FastNoiseLite, flat world (нет руд) |
| SimulationCore | C++ | ✅ | `:4000` (router, нет собственного RPC-порта) | ECS (EnTT), MachineSystem 20Hz tick, multiblock detection, RecipeManager integration, PipeEnergyClient (публикует energy.node.update → PipeNetwork, асинхронный consume request/response, обработка energy.flow). World→ECS registration не сделана. Дополнительно: Overheat detection (WARNING/CRITICAL), Machine explosion, Boiler heat/water→STEAM conversion, MachineSystem 50% speed at WARNING |
| GameClient | C++ | 🟡 | `:7777` (gw ctrl), `:7778` (gw bulk) | bgfx, GLFW, ImGui, FPS cam, DDA raycast, break/place, workbench 3×3, BlockEntityUpdate handler |
| MetaDB | Go | ✅ | `:5005` (JSON API), `:5006` (FlatBuffers RPC) | SQLite, подключён к MessageRouter. Player joined/left end-to-end (Gateway→`player.joined`/`player.left`→MetaDB) |
| EntityStateStore | C++ | ✅ | `:5200` (RPC), `:4000` (router) | LMDB-backed entity persistence, 11 файлов, pub/sub `entity.state.get/set`, ChunkStoreClient |
| **RecipeManagerLib** | C++ | ✅ | `src/libs/recipe_manager_lib/` | Shared library (9 файлов). JSON-рецепты (6 типов), ConditionEvaluator с MachineState из ECS. Выделен из SimulationCore. |
| PipeNetwork | C++ | ✅ | `:—` | BFS граф (PipeNetworkManager, distributeEnergy/distributeFluid), MessageRouter integration — подписка на `energy.node.update`, `energy.consume.request`, `energy.check.request`; публикация `energy.consume.response`, `energy.flow`. Дополнительно: Cable overheat detection (WARNING/CRITICAL), Cable explosion, Fluid heat/water→STEAM conversion, ItemPipe network support |
| SpatialIndex | C++ | 🔴 | `:—` | **Не начат** (main.cpp = 51 байт). L2 deferred. |
| ChestSync | C++ | ✅ | `:—` | ChestOpenReq/Resp protocol, gateway+netclient routing, chest window network sync |
| DrillSystem | C++ | ✅ | `:—` | Autonomous Mining MVP — DrillSystem with spiral BFS, mining progress, output buffer, energy consumption |

## FlatBuffers схемы

| Файл | Статус | Сообщения |
|------|--------|-----------|
| `protocol/core.fbs` | ✅ | Vec3i, Vec3f, ItemStack, PlayerAction, ChunkData, EntitySnapshot, BlockChangedEvent, BlockEntityUpdate, MultiblockCreatedEvent, InventoryUpdate |
| `protocol/gateway.fbs` | ✅ | GatewayPayload union (types 0-10), GatewayMessage |
| `protocol/chunkstore.fbs` | ✅ | GetBlock/SetBlock/GetChunk/SaveChunk RPC |
| `protocol/simcore.fbs` | ✅ | BlockChangedReq, MatchPatternReq, TickReq + resp |
| `protocol/recipe.fbs` | ✅ | CheckRecipeReq, CraftReq, EvaluateConditionsReq, MachineType enum (NONE..CHEMICAL_REACTOR) |
| `protocol/entity_state_store.fbs` | ✅ | GetEntityStateReq/Resp, SetEntityStateReq/Ack RPC |
| `protocol/tile_entity_store.fbs` | ✅ | TileEntity save/load RPC |
| `protocol/item_registry.fbs` | ✅ | ItemRegistry sync RPC |
| `protocol/machine_state.fbs` | ✅ | MachineState RPC |
| `protocol/meta_db.fbs` | ✅ | Player inventory/position/state RPC |

## Протокол клиент-гейтвей

```
[4B size BE][1B msg_type][FlatBuffer]
  0 = PlayerAction         (client→gateway)
  1 = ChunkData            (gateway→client) — full chunk on load/resync
  2 = EntitySnapshot       (gateway→client)
  3 = BlockAck             (gateway→client) — commit confirmation/rejection
  4 = SetBlockAction       (client→gateway) — CAS block placement
  5 = InventoryUpdate      (gateway→client) — inventory snapshot/update
  6 = InventoryAction      (client→gateway) — inventory operation
  7 = CraftRequest         (client→gateway) — craft from workbench (io_uring)
  8 = CraftResponse        (gateway→client) — craft result (io_uring)
  9 = GridUpdate           (gateway→client) — workbench grid state
  10 = BlockEntityUpdate   (gateway→client) — machine/multiblock state
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

# Этап 1: Стабилизация геймплея 🟡

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
- `WorldContainerInventory::storage_` = `nullptr` — персистентность не подключена
- Wire `storage_` → EntityStateStore RPC
- См. item-inventory continuation spec

### 1.3 Chunk versioning (опционально)
- ChunkStore хранит `version:uint64`, инкрементит при `SetBlock`
- Клиент игнорирует чанки с version ≤ текущей
- **Приоритет:** низкий (нет сетевых race)

---

# Этап 2: Мультиблоки 🟡

**Цель:** поставить 3×3×3 специальных блоков → SimulationCore находит паттерн → создаёт MultiblockController.

## Реализовано

- [x] **Pattern matching** — `SimulationEngine.cpp:matchElectrolyser`, `registerController`
- [x] **SetBlockMeta RPC** — SimulationCore пишет `mb_id` в meta-layer ChunkStore
- [x] **ECS компоненты** — `MachineComponent`, `RecipeProgress`, `InventoryContainer`, `EnergyStorage`
- [x] **MachineSystem** — 20Hz tick, recipe matching, energy consumption, progress
- [x] **BlockEntityUpdate протокол** — FlatBuffers таблица (hatches, covers, fluids, mb_id, structure_valid, network_id), тип 10 в gateway.fbs

## Осталось (continuation spec)

1. **World→ECS регистрация** — при `SetBlockAction` создавать ECS entity с `MachineComponent`
2. **Machine GUI** — client handler для `BlockEntityUpdate` (progress bar, energy, slots)
3. **Multiblocks L2** — формирование/разрыв, сохранение через EntityStateStore

**Spec:** `doc/EPICS/1-gameplay-machines-multiblocks/1-gameplay-machines-multiblocks.md`

---

# Этап 3: PipeNetwork 🟡

**Цель:** трубы проводят энергию/жидкость/предметы. Строишь трубы → PipeNetwork решает граф → flow_map.

## Реализовано

- [x] **BFS граф** — `PipeNetworkManager` в `PipeNetwork.h/.cpp`
- [x] **distributeEnergy / distributeFluid** — базовые алгоритмы распределения
- [x] **MessageRouter integration** — подписка `energy.node.update`, `energy.consume.request`, `energy.check.request`; публикация `energy.consume.response`, `energy.flow`; обработчики `handleConsumeRequest`, `handleEnergyFlow`
- [x] **Tick integration с SimulationCore** — SimulationCore публикует `energy.node.update`, отправляет `energy.consume.request` через PipeNetwork, получает `energy.consume.response` и `energy.flow`
- [x] **CableGraph** — пакетная маршрутизация электричества по кабелям (BFS, voltage/ampacity, overheat)
- [x] **Item Pipe Network** — `moveItemsInNetwork/tickItemNetworks/findNextItemHop` — перемещение предметов через BFS от source к sink
- [x] **Fluid Protocol + Service** — подписка на `fluid.node.update/check.request/consume.request`, хендлеры `handleFluidNodeUpdate/Check/Consume`
- [x] **FluidClient** — `Network/FluidClient.h/.cpp` для SimulationCore (публикация fluid.node.update, fluid.consume.request)
- [x] **FluidRegistry** — инициализация дефолтных жидкостей (water=84, steam=85, sulfuric_acid=86)
- [x] **FlatBuffers Item Protocol** — `ItemNodeUpdate`, `ItemTransferReq/Resp`, `ItemFlowEvent` в `pipe_network.fbs`
- [x] **Transformer ECS** — `TransformerComponent` + `TransformerSystem` для блоков 72-73 (MV/HV step-up/down)
- [x] **PipeMeshBuilder fix** — исправлены ID блоков (ITEM_PIPE→62, FLUID_PIPE→61, кабели 66-71)

## Осталось

- [ ] Client визуализация (цвет труб, соединения)
- [ ] Multi-dimension (2 инстанса)

**Spec:** `doc/EPICS/0-foundation-energy-fluids/0-foundation-energy-fluids.md`

---

# Этап 4: SpatialIndex 🔴 (L2, deferred)

**Цель:** быстрые пространственные запросы — R-tree для мультиблоков, Octree для entity.

**Статус:** `src/services/spatial_index/main.cpp` = 51 байт. Не начат.

- R-tree (`bgi::rtree<AABB>`) для bounding box мультиблоков
- Dynamic octree для entity queries
- RPC: FindInRadius, FindAtPoint, FindEntitiesInAABB
- SimulationCore использует SpatialIndex для multiblock queries

**Причина defer:** Current multiblock queries работают напрямую в SimulationCore.
SpatialIndex понадобится при 100+ мультиблоков в одном чанке.

---

# Этап 5: GameClient улучшения 🔴

**Цель:** клиент перестаёт быть tech-demo.

| Задача | Статус |
|--------|--------|
| Block atlas (UV-координаты) | 🔴 |
| Hotbar + block picking | 🔴 |
| Sound (miniaudio) | 🔴 |
| Drag-and-drop в инвентаре | 🔴 |
| Pause menu, settings | 🔴 |

---

# Этап 6: Инфраструктура 🔴

- Dependency management (Conan/vcpkg — единый подход)
- CI (GitHub Actions сборка + тесты)
- Packaging (AppImage / Docker compose)
- Graceful shutdown, health checks

---

# Этап 7: Будущее ⏸

- Entity system (мобы, игроки в ECS)
- Physics (гравитация, коллизия)
- Networking v2 (LZ4, rate limiting, reconnection, channels)
- Mod runtime (C++ `.so`/`.dll`, Lua/Python)
- AssetServer
- Scale (HTTP/3 + QUIC, шардирование, web client)

---

# Этап 8: Crafting Pipeline 🟡

**Цель:** крафт через RecipeManager.

## Реализовано

- [x] **GridPatternMatcher** — shape-aware 3×3 matching с rotation/reflection (8 трансформаций)
- [x] **CraftRequestHandler** — io_uring, protocol types 7/8, shape-aware matching
- [x] **RecipeManager → shared library** — `src/libs/recipe_manager_lib/` (9 files)
- [x] **ConditionEvaluator** — заполняется MachineState из ECS (`RecipeManager.cpp` overload). Больше не пустой placeholder.
- [x] **CraftResponse UI feedback** — цветной текст + таймер в WorkbenchWindow
- [x] **6 recipe JSONs** — все типы машин (кроме macerator.json)
- [x] **MachineState struct** — `id, inventory[9], output, tickRate`

## Осталось

- [ ] **Macerator recipes** — `data/recipes/macerator.json` отсутствует
- [ ] **Server-authoritative grid state** — WorkbenchStateManager не подключён к EntityStateStore RPC
- [ ] **Inventory consumption** — `publishInventoryUpdate()` шлёт пустой `InventoryUpdate` без дельты
- [ ] **Drag-and-drop** — idle → drag_start → drag → drop state machine

**Specs:** `doc/EPICS/0-foundation-item-inventory/0-foundation-item-inventory.md`

---

# Этап 9: Инвентарная система 🟡

## Реализовано

- [x] **NetClient handlers** — InventoryUpdate (type 5), InventoryAction (type 6)
- [x] **Gateway forwarding** — pub/sub `player.inventory.update` → MetaDB, `player.inventory.action` → SimulationCore
- [x] **MetaDB pub/sub** — `PublishInventoryUpdate()`, `PublishInventoryAction()`
- [x] **EntityStateStore RPC** — GetEntityStateReq/Resp, SetEntityStateReq/Ack (LMDB, :5200)
- [x] **Player joined/left** — end-to-end (Gateway `player.joined`/`player.left` → MetaDB handlePlayerJoined/handlePlayerLeft)
- [x] **PlayerInventory UI** — базовое окно (E key), 27 слотов

## Осталось

- [ ] Drag-and-drop state machine
- [ ] Выгрузка crafted items в player inventory
- [ ] Синхронизация с MetaDB при коннекте

---

# Архитектурные заметки

## Work tracking

Active work is tracked as **openspec changes** (`openspec/changes/<id>/`). Each change has a `proposal.md` (why/what), `tasks.md` (checklist), and formal spec deltas.

```
openspec/changes/
├── complete-electric-tools-wrench/   # 🟡 WIP — raycast, persistence, textures
├── implement-pipes-cables-transport/ # 🟡 WIP — item/fluid/energy transport
├── implement-ore-generation/         # 🔴 Not started — sinusoidal veins
├── implement-multiblocks-l2/         # 🔴 Not started — EBF, SpatialIndex, patterns
├── implement-questbook/              # 🔴 Not started — progression system
└── complete-autonomous-mining/      # 🟡 WIP — persistence, UI, pipes
```

Completed / superseded specs live in `doc/EPICS/archive/`.

## Где что лежит

| Что нужно сделать | Куда идти |
|------------------|-----------|
| Добавить FlatBuffers сообщение | `src/protocol/*.fbs` |
| Изменить протокол клиент-гейтвей | `gateway/main.cpp`, `game_client/Network/NetClient.*` |
| Player save/load | `meta_db/main.go` |
| Gateway форвардинг | `gateway/message_router_client.*`, `gateway/public_server.*` |
| Логика чанков | `chunk_store/`, `game_client/World/`, `game_client/Cache/` |
| Симуляция / ECS | `simulation_core/` |
| Energy / liquids | `pipe_network/` |
| Spatial queries | `spatial_index/` (L2 deferred) |
| Client UI / Render | `game_client/UI/`, `game_client/Render/` |
| Recipe system | `libs/recipe_manager_lib/` |
| Entity persistence | `entity_state_store/` |

## Сборка

```bash
# Release
conan install . --build=missing
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Debug
conan install . --build=missing
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

## Запуск (порядок важен)

```bash
./build/routerd          # 1. Pub/sub
./build/chunkd           # 2. World
./build/entitystated     # 3. Entity state
./build/gatewayd         # 4. Gateway
./build/simcored         # 5. Simulation
./build/meta-dbd         # 6. Player DB
./build/pipe_networkd    # 7. PipeNetwork
./build/client           # 8. Client

# spatial-index — заглушка, не запускать
```

## Известные проблемы

| Проблема | Где | Статус |
|----------|-----|--------|
| Gateway шлёт лишние чанки | `public_server.cpp` | 🔴 TODO |
| Inventory persistence (storage_ = nullptr) | `WorldContainerInventory` | 🔴 TODO |
| Drag-and-drop не реализован | `SlotGrid/` | 🔴 TODO |
| Inventory sync с MetaDB при коннекте | `MetaDB/` | 🔴 TODO |
| Macerator recipes отсутствуют | `data/recipes/` | 🔴 TODO |
| ConditionEvaluator MachineState gap | **ConditionEvaluator.cpp** | ✅ DONE (RecipeManager.cpp overload) |
| CraftResponse UI feedback | **WorkbenchWindow** | ✅ DONE |

## Ключевые архитектурные решения

### Energy = число, fluids = ItemStack
- EnergyStorage — ECS-буфер. PipeNetwork считает поток.
- Fluids/gas/plasma — как `ItemStack`, не отдельный тип. Нет `FluidTankComponent`.

### RecipeManager — shared library
- Вынесен из SimulationCore в `src/libs/recipe_manager_lib/` (9 файлов)
- Не отдельный RPC-сервис на MVP

### BlockEntityUpdate — FlatBuffers table
- В `core.fbs` с HatchInfo, CoverInfo, FluidTank, mb_id, structure_valid, network_id
- Тип 10 в `gateway.fbs` GatewayPayload

### MessageRouter: io_urning вместо NATS/Redis

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
**Updated:** 2026-06-28 — Completed EPICs archived, WIP tracked as openspec changes.
