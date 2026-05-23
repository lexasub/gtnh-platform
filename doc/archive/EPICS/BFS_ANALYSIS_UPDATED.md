# BFS-анализ всех эпиков (актуализирован)

> Обновление от 2026-06-27 — CreativeGenerator зарегистрирован + CableGraph вшарен + WrenchHandler end-to-end.
> Изменения относительно версии 2026-06-22 выделены **жирным**.

---

## Сводка сессии 2026-06-27: Три орфана починены

**Проделано:**

### 1. CreativeGenerator — зарегистрирован в tick loop ✅
- Был: `CreativeGeneratorSystem` существовал (56 строк, 1024 EU/t), но `registerSystem()` не был вызван в main.cpp
- CSV-данные были несогласованы: `kCreativeGeneratorBlockId=63`, но items.csv имел creative_generator на id=61, producers.csv на id=51, machines.csv на id=46. `machine_id` никогда не совпадал с 63.
- **Фикс**: вставлен `registerSystem` после GeneratorSystem (main.cpp:234). Swapped creative_generator↔fluid_pipe в items.csv. Обновлены producers.csv (51→63), machines.csv (46→63), удалён дубликат из consumers.csv.
- **Результат**: CreativeGenerator теперь тикает, даёт 1024 EU/t, block_id=63 консистентен везде.

### 2. CableGraph — registerGenerator/registerMachine вшарены ✅
- Был: `CableGraph::registerGenerator/registerMachine` никем не вызывались (dead code). GeneratorSystem публиковал `publishNodeUpdate(is_source=true)` только для HEAT, не для ELECTRICITY.
- **Фикс**: Добавлены `unregisterGenerator/unregisterMachine` в CableGraph. В `PipeNetworkService::handleNodeUpdate` добавлен вызов `registerGenerator/registerMachine` для ELECTRICITY узлов. GeneratorSystem теперь публикует `is_source=true` и для ELECTRICITY.
- **Результат**: CableGraph получает узлы, может строить граф и маршрутизировать пакеты.

### 3. WrenchHandler — end-to-end wiring через 8 файлов ✅
- Был: `WrenchHandler` существовал (48 строк), `SideConfig` цикл работал, ToolAction/ToolActionResp протокол был в core.fbs — но **ничего не было вшарено**: никакой код не вызывал WrenchHandler, Gateway не обрабатывал kToolAction, клиент не имел SendToolAction.
- **Фикс**:
  - `gateway.h/cpp`: константа kToolAction=13 + switch case → publish `"player.tool.action"`
  - `gateway/main.cpp`: subscribe `"player.tool.action.response"` + route
  - `core.fbs`: `all_roles:[uint8]` в ToolActionResp
  - `NetClient.h/cpp`: `SendToolAction()` + `ToolActionRespCallback` + обработка ответа
  - `InteractionSystem.cpp`: клавиша G → `SendToolAction(WRENCH_CYCLE)`
  - `simcore/main.cpp`: subscribe `"player.tool.action"` + хендлер (WrenchHandler::cycleFace → publish response)
- **Результат**: WrenchHandler работает от клавиши G на клиенте до ответа от SimulationCore. SideConfig циклически переключается (INPUT→OUTPUT→ENERGY→FLUID_IN→FLUID_OUT→ANY→NONE).

---

## 1. `0-basic-mechanics/` — Basic Mechanics

**Статус**: ✅ **В архиве. Всё сделано.**

---

## 2. `0-foundation-energy-fluids/` — Energy & Fluids

**Статус**: **🟢 L1 — всё сделано** (continuation spec active, L2 deferred)

**Что изменилось с 2026-06-11:**
- ❌ ~~"PipeNetwork → MessageRouter (сетевой слой)"~~ → ✅ **DONE**
- ❌ ~~"PipeNetwork → SimulationCore (интеграция)"~~ → ✅ **DONE**
- ❌ ~~"world.blocks.changed pipe auto-detection"~~ → ✅ **DONE** (PipeNetworkService.h/.cpp, isPipeBlock())
- ❌ ~~"fluids as items (без FluidTankComponent)"~~ → ✅ **Решение принято, архитектура определена**

**Новое с 2026-06-22:**
- **CableGraph.cpp/h** — ✅ починены namespace, pragma once, 5 багов (pair→array, z-coord, sourceCableNode scope, sign-compare, includes). **pipe_networkd собирается чисто.**
- **PipeNetworkService::isCableBlock** — ✅ исправлена рекурсия (namespace clash с `gtnh::pipe_network::isCableBlock`)

**L1 Checklist** (continuation spec) — **ALL DONE**:
1. ✅ CreativeGenerator configurable (CreativeGeneratorSystem, ID 63)
2. ✅ PipeNetwork → MessageRouter
3. ✅ PipeNetwork → SimulationCore (energy.consume.request/response + energy.flow)
4. ✅ world.blocks.changed subscription (auto pipe detection)
5. ✅ Fluids as items — решение принято, отдельного тикета не требует
6. ✅ ConditionEvaluator — MachineState заполняется из ECS

**Вывод**: Эпик L1 завершён. Continuation spec можно архивировать при желании — L2 deferred без конкретных задач.

---

## 3. `0-foundation-item-inventory/` — Item & Inventory

**Статус**: **🟢 DONE — Архив** (ранее: 🟡 "2 задачи осталось")

**Что изменилось:**
- **Server-authoritative grid state через EntityStateStore** ✅ — WorkbenchStateManager переписан: 9-slot grid ↔ 45-byte blob, EntityStateStoreClient RPC
- **Inventory consumption через MetaDB** ✅ — после `recipe->craft(grid)`, deduct из `g_inventories[playerId]`, persist + publish `player.inventory.update`

**Архив**: `doc/EPICS/archive/0-foundation-item-inventory/` — обновлён (2 задачи помечены done)

---

## 4. `1-gameplay-machines-multiblocks/` — Machines & Multiblocks

**Статус**: **🟡 L1 — MachineSystem + GUI + multiblock→reciped сделаны. L2 — deferred.**

**Новое с 2026-06-11:**

- ✅ **Machine GUI** — `MachineWindow::Render()` data-driven от `MachineRegistry` (слоты, прогресс, энергия)
- ✅ **MachineSystem refactoring** — 3-pass tick (Pass 0: reciped publishing, Pass 1: local recipe, Pass 2: tick+energy). `managed_externally` флаг для multiblock routing.
- ✅ **Multiblock→reciped flow** — `SimulationEngine::onMachineCreated` callback → `world.block_entity.update` → reciped. Hash-gated inventory publishing предотвращает loops.
- ✅ **BlockEntityUpdate protocol** — FlatBuffers таблица с pos/machine_type/progress/energy/items/mb_id
- ✅ **EntityStateStore** — C++ сервис (LMDB, TCP :5200, entity.state.get/set), WorkbenchStateManager использует для grid persistence

**Новое с 2026-06-20 (heat_generator → heat_furnace dogfooding):**

- ✅ **EnergyType из MachineRegistry** — `SimulationEngine::onBlockChanged` теперь читает `energy_in`/`energy_out` из реестра. Heat-машины (36, 46, 48) получают `EnergyType::HEAT` вместо ELECTRICITY.
- ✅ **HeatTransferSystem** — новая ECS-система (регистрируется до MachineSystem). Для каждого heat-consumer'а проверяет 6 соседей, переносит тепло из adjacent generator'а. MachineSystem пропускает PipeNetwork для HEAT-типа.
- ✅ **BlockEntityUpdate теперь включает energy_capacity** — `IEventPublisher::publishBlockEntityUpdate` получил параметр `energy_capacity`, публикуется в FlatBuffer. Client MachineWindow использует для отрисовки шкалы.
- ✅ **Client: kBlockEntityUpdate routing** — отдельный коллбэк `onBlockEntityUpdate_` в NetClient, проводка к `uiMgr_.HandleNetwork(GatewayMsg::kBlockEntityUpdate, ...)`. MachineWindow парсит FlatBuffer вместо 8-байтового кастомного формата.
- ✅ **Client: MachineRegistry инициализация** — `GameClient::Init()` загружает `consumers.csv`/`producers.csv`, вызывает `BlockUIFactory::LoadFromRegistry`. Все машины (включая heat) теперь открывают MachineWindow по клику. Слоты показывают предметы из `BlockEntityUpdate`.
- ✅ **GeneratorSystem: расширенные FuelValues** — добавлены oak_planks (13, 2000) и stick (32, 500) к coal (44, 8000).

**Обновление 2026-06-22 (codebase analysis — декомпозиция L1):**

Machine slot interaction и server machine registration **разбиты на 10 задач** с приоритетами:
→ `tasks/README.md` — полный порядок выполнения

**L1 — Machine slot interaction (1 задача):**
- ⚠️ 90% **уже работает**: `SetMachineSlotReq` в core.fbs, GatewayMsg.kSetMachineSlot=15, Gateway публикует `player.machine.slot`, SimulationCore main.cpp (lines 382-436) полный handler, MachineWindow вызывает `SendSetMachineSlot()`
- ❌ Единственный gap: **нет `SetMachineSlotResp`** в core.fbs. Клиент не получает явного ACK — только BlockEntityUpdate.
- 📋 Task: `01-set-machine-slot-resp-protocol.md` → `02-set-machine-slot-resp-simcore.md` → `03-set-machine-slot-resp-client.md`

**L1 — Server machine registration (7 задач):**
- ❌ `isMachineBlock()` — **хардкод** block_id вместо MachineRegistry lookup. Unregistered blocks (crafting_table, tools, chest) получают MachineComponent — баг.
- ❌ `defaultMachineSlotCount()` — **хардкод** вместо чтения из MachineRegistry (13 машин уже имеют слоты в CSV)
- ❌ EnergyStorage — не инициализируется из MachineRegistry (capacity, tier, maxInput/maxOutput)
- ❌ Нет валидации: "этот block_id — машина?" через единый registry lookup
- ❌ ESS save/remove — не подключён для machine entity lifecycle

**L2 — что всё ещё не сделано:**
- ❌ Multiblocks — полная gameplay логика (`matchElectrolyser()` wiring, SpatialIndex, persistence)
  - SpatialIndex: `int main() { return 0; }` — полный stub
  - Только ELECTROLYSER_PATTERN (хардкод 3x3x3), нет EBF (3x3x4), Boiler, LCR
  - Нет sim.multiblock.* топиков в MessageRouter
  - Нет dissociation, hatch detection
  - → вынесено в `doc/EPICS/7-multiblocks-l2/`

---

## 5. `2-infrastructure-shared-recipe-lib/` — Shared RecipeManager Library

**Статус**: ✅ **В архиве. Рефакторинг выполнен.**

---

## 6. `3-infrastructure-automation/` — Infrastructure & Automation

**Статус**: 🟡 Инфра — сделана, Автоматизация — отложена

**Без изменений:**
- MessageRouter (Go) — ✅ pub/sub, service discovery, heartbeat
- EntityStateStore (C++) — ✅ отдельный сервис с LMDB
- RecipeManager — ✅ standalone RPC + embedded (через recipe_manager_lib)
- MetaDB — ✅ базовый, player.joined/left — end-to-end
- WorldGenerator — ✅ (4 файла), **но руды не генерируются**
- ❌ ItemTransporter (хопперы) — отложено
- ❌ Логистические сети — отложено
- ❌ Редстоун — отменено

---

---

## Новые эпики (из userflow gap analysis)

### `4-electric-tools-wrench/` — Электрические инструменты и конфигурация машин

**Статус**: 🔴 Не начато  
**Userflows**: `doc/userflow/08-wrench-tools-config.puml` (3 диаграммы)

Раздел A: Электрические инструменты (дрели, пилы, батареи, зарядные устройства)
- Блоки: battery_buffer (LV/MV/HV), charger
- Предметы: drill_ulv/lv/mv/hv, chainsaw_lv, wrench
- Mining level по tier (ULV: stone, LV: iron_ore, HV: diamond)
- Зарядка от PipeNetwork через BatteryBuffer

Раздел B: Wrench / Machine Side Config
- side_config: array<uint8, 6> в MachineComponent
- 6 граней, 7 ролей (INPUT/OUTPUT/ENERGY/FLUID_IN/FLUID_OUT/ANY/NONE)
- Циклическое переключение: ПКМ ключом
- PipeNetwork BFS routing по side_config

### `5-transport-pipes-cables/` — Трубы и кабели

**Статус**: 🔴 Не начато  
**Userflows**: `doc/userflow/06-item-energy-transport.puml` (3 диаграммы)

Раздел A: Item Pipes — BFS от output к input, 1 block/tick, item buffer
Раздел B: Fluid Pipes — fluids-as-items, второй граф PipeNetwork, гравитация
Раздел C: Energy Cables — voltage tier check, cable loss, overheat→explosion, transformers

### `6-world-ore-generation/` — Генерация руд (Section E из 3-infra)

**Статус**: 🔴 Не начато  
**Userflows**: `doc/userflow/07-ore-processing-chain.puml` (O1)

- Синусоидальные жилы через 3D noise
- 8 типов руд (iron, copper, tin, coal, gold, redstone, lapis, diamond)
- Разные высоты генерации per ore type
- Без инструментов: руда = блок (MVP)

### `7-multiblocks-l2/` — Multiblocks полный gameplay (L2)

**Статус**: 🔴 Не начато  
**Userflows**: `doc/userflow/09-multiblocks.puml` (3 диаграммы)

Раздел A: SpatialIndex integration
Раздел B: EBF — heating coils, heat requirement
Раздел C: Large Steam Boiler — firebox, water→steam
Раздел D: LCR — fluid + solid recipes
Раздел E: Dissociation — ключевой блок → очистка

---

## Архивы

### `archive/0-foundation-recipe-system/` — ✅ Корректно

### `archive/1-gameplay-player-crafting/` — ✅ Корректно
4 задачи resolved:
- CraftResponse UI feedback → ✅ DONE
- Server-authoritative grid state → ✅ **теперь DONE** (через EntityStateStore)
- Inventory consumption → ✅ **теперь DONE** (через MetaDB)
- ConditionEvaluator MachineState → ✅ DONE (из ECS)

### `archive/2-infrastructure-shared-recipe-lib/` — ✅ Корректно

### `archive/0-foundation-energy-fluids/` — ✅ Корректно

### `archive/0-foundation-item-inventory/` — ✅ **Обновлён** (2 remaining tasks → DONE)

---

## Сводная таблица (актуальная)

| Эпик | Статус | Архив? | Что осталось |
|------|--------|--------|-------------|
| `0-basic-mechanics` | ✅ **В архиве** | ✅ Да | — |
| `0-foundation-energy-fluids` | 🟢 **L1 done** | **Можно** | L2 → 5-transport-pipes-cables |
| `0-foundation-luanti-patterns` | ✅ **В архиве** | ✅ Да | — |
| `0-foundation-item-inventory` | ✅ **DONE** | ✅ **Да** | **0 задач** |
| `1-gameplay-machines-multiblocks` | 🟡 **L1 WIP** | Нет | 10 задач: T01 SetMachineSlotResp (core.fbs ✅) + T02 simcore handler + T03 client handler. T04-T10 machine registration (isMachineBlock/EnergyStorage — DONE ✅). SetMachineSlotResp simcore/client handler pending. |
| `2-infrastructure-shared-recipe-lib` | ✅ **В архиве** | ✅ Да | — |
| `3-infrastructure-automation` | 🟡 Инфра готова | Частично | Руды → `6-world-ore-generation`, Automation deferred |
| **`4-electric-tools-wrench`** | 🟡 **WIP — wiring DONE** | Нет | A1 ToolIds.h ✅, A2 ToolAction protocol ✅, A4 ItemEnergyStorage ✅, A5 MiningCalculator ✅. **WrenchHandler end-to-end wiring ✅ (2026-06-27)** — Gateway (kToolAction=13), SimulationCore (handler), Client (G key → SendToolAction). B1 SideConfig ✅, B4 WrenchHandler ✅. **Pending**: A6 BatteryBufferSystem, A8 Tooltip, B3 client raycast face detection, electric tool items (drills, batteries). |
| **`5-transport-pipes-cables`** | 🟡 **WIP — CableGraph wired** | Нет | Task1 pipe IDs+isPipeBlock ✅, Task2 item graph (pending), Task8 fluid IDs+FluidRegistry ✅, Task14 cable IDs+CableTypes ✅, **Task15 CableGraph ✅ (2026-06-22 namespace fixes + 2026-06-27 wiring: registerGenerator/registerMachine теперь реально вызываются через PipeNetworkService::handleNodeUpdate + GeneratorSystem публикует ELECTRICITY)**, T16 overheat+T17 cable loss+T4 move (pending). Task9 fluid graph+tasks 10-13 not started. |
| **`6-world-ore-generation`** | 🟡 **WIP** | Нет | Task1 ore IDs ✅, Task2 ores.json+OreConfig ✅, Task3 OreGenerator ✅, **Task5 WorldGenerator integration ✅ (OreConfig::load добавлен 2026-06-22)**, Task6 ore drop (pending), Task7 ChunkStore persist (verified — stores uniformly). |
| **`7-multiblocks-l2`** | 🔴 **Новый** | Нет | EBF, Large Boiler, LCR, SpatialIndex, Dissociation |

## Решённые открытые вопросы

| Вопрос | Решение |
|--------|---------|
| Q1 — кто наполняет EnergyStorage | CreativeGenerator configurable |
| Q2 — Energy: ECS или PipeNetwork | PipeNetwork считает поток |
| Q3 — FluidSlots в MachineState | Не нужно — fluids as items |
| Q4 — FluidTank/FluidStack .fbs dead code | Оставить для UI баков |
| Q5 — PipeNetwork сервис или lib | Отдельный сервис |
| Q6 — SpatialIndex на L1 | Не нужен (L2) |
| Q7 — MetaDB login/logout | Уже end-to-end |
| Q8 — BlockEntityUpdate формат | FlatBuffers табл. с hatches/covers |
| Q10 — Recipe auto-selection | UI как GTNH |
| Q11 — Fluid flow: соседи или граф | PipeNetwork + infinite placeholder |

**Grooming deferred**: Q12 (single-thread tick), Q13 (ImGui sync) — при 100+ машинах.
**Post-MVP**: Q9 (Inventory Actions breaking change).
