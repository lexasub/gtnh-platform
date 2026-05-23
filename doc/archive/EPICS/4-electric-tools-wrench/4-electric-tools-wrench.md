# EPIC: Electric Tools & Machine Config

**Layer**: L2  
**Статус**: 🟡 **WIP — Electric Tools A-часть DONE, Wrench B-часть DONE**  
**Последнее обновление**: 2026-06-28  
**Зависимости**: Inventory system, EnergyStorage component, MachineComponent (side_config ✅)

## Userflow диаграммы

- `doc/userflow/08-wrench-tools-config.puml` — W1: Wrench config, W2: Electric tools, W3: Machine side setup

## Обзор

Два связанных направления:
1. **Electric tools** — дрели, пилы, батареи, зарядные устройства (charging)
2. **Wrench / Machine Config** — переназначение сторон машин (INPUT/OUTPUT/ENERGY/FLUID)

## Текущее состояние (из кода): **Wiring DONE, A-часть pending**

После аудита 2026-06-27 и полной имплементации wiring:

| Что искали | Результат (2026-06-27) |
|-----------|----------------------|
| ToolAction протокол + обработчик | ✅ **DONE** — kToolAction=13 в Gateway, ToolAction/ToolActionResp в core.fbs, сallbacks в NetClient, хендлер в simcore (WrenchHandler::cycleFace) |
| Wrench end-to-end (G key → response) | ✅ **DONE** — InteractionSystem (G key) → NetClient.SendToolAction → Gateway → SimulationCore → WrenchHandler → response → Gateway → Client callback |
| MachineComponent::side_config | ✅ **Exists** — `std::array<uint8_t, 6>` в MachineComponent.h (7 ролей: INPUT/OUTPUT/ENERGY/FLUID_IN/FLUID_OUT/ANY/NONE) |
| ToolIds.h | ✅ **Exists** — WRENCH_CYCLE, MODE_NEXT, SIDE_INPUT, etc. |
| WrenchHandler | ✅ **Exists** — WrenchHandler.h/.cpp (48 строк, cycleFace циклически переключает роли) |
| SideConfig cycle | ✅ **DONE** — WRENCH_CYCLE → 5 ролей последовательно (INPUT/OUTPUT/ENERGY/FLUID_IN/FLUID_OUT/ANY/NONE) |
| DrillSystem (ECS) | ✅ **DONE** — 2026-06-28: Полная имплементация автономного бурения (DrillComponent, DrillSystem, спиральный BFS, прогресс добычи, output buffer, энергопотребление) |
| ItemEnergyStorage | ✅ **DONE** — 2026-06-28: Реализован в ItemEnergyStorage.h, константы для drill_ulv/lv/mv/hv |
| Tool energy tooltips | ✅ **DONE** — 2026-06-28: SlotGrid.cpp показывает заряд инструментов в тултипе |
| Drill item registration | ✅ **DONE** — consumers.csv ID 61-64, items.csv ID 100-103 |
| BatteryBufferSystem | ✅ **DONE** — 2026-06-28: Подключён к PipeEnergyClient |
| EnergyStorage для предметов | ❌ **Pending** — только для машин, не для items |
| Battery buffer blocks | ❌ **Pending** — не зарегистрированы |
| item_id для drill/wrench/chainsaw/battery | ❌ **Pending** — 63 предмета, ни одного инструмента |
| Client raycast face detection | ❌ **Pending** — B3: нужно определить грань при нажатии G |

**Вывод**: Раздел A (Electric Tools) **DONE** — DrillSystem, ItemEnergyStorage, BatteryBufferSystem, tooltips. Раздел B (Wrench/SideConfig) **DONE end-to-end**, но без raycast-определения грани — текущая имплементация просто циклически перебирает все 6 граней. B3 (raycast) — **Pending**.

---

## Раздел A: Electric Tools

### Что нужно сделать

Электрические инструменты с системой энергии, mining level по tier, зарядкой от PipeNetwork — **все реализовано** (DrillSystem, ItemEnergyStorage, BatteryBufferSystem, tooltips, энергопотребление, прогресс добычи).

### Блоки

| Block ID | Название | Назначение |
|----------|----------|------------|
  | 104 | battery_buffer_lv | Зарядка LV инструментов |
  | 105 | battery_buffer_mv | Зарядка MV инструментов |
  | 106 | battery_buffer_hv | Зарядка HV инструментов |
  | 107 | charger | Трансформерная зарядка |

### Предметы (Items)

| Item ID | Название | Тип |
|---------|----------|-----|
  | 100 | drill_ulv | Electric tool — ULV |
  | 101 | drill_lv | Electric tool — LV |
  | 102 | drill_mv | Electric tool — MV |
  | 103 | chainsaw_lv | Electric tool — LV |
  | 108 | wrench | Wrench tool |

### Где смотреть

| Файл | Роль |
|------|------|
| `src/data/registry/items.csv` | Регистрация предметов-инструментов |
| `src/data/registry/items.db` | SQLite база предметов |
| `src/protocol/core.fbs` | ToolAction — есть ли уже? |
| `src/services/simulation_core/main.cpp` | ActionDispatcher — обработка ToolAction |
| `src/services/simulation_core/ECS/Components/EnergyStorage.h` | Инструменты имеют EnergyStorage (через компонент или item meta) |
| `src/services/chunk_store/` | CAS для добычи блоков инструментом |
| `src/services/game_client/` | Client-side — отображение заряда, эффективность добычи |

### Архитектура

**Mining by tier:**
- ULV: stone, gravel
- LV: iron ore, copper ore, tin ore
- MV: gold ore, redstone, lapis
- HV: diamond, titanium, tungsten

**Tool energy:**
- Drill имеет внутреннюю батарею (EnergyStorage в item)
- mining_cost = hardness * tier_multiplier
- При depletion — drill не ломает блоки

**Charging:**
- Battery Buffer — блок, подключается к PipeNetwork
- Кладёшь drill в слот — каждый тик drill.energy += charge_rate
- Зарядка останавливается при полном заряде или пустом буфере

### Критерии готовности

- [x] Item registration для drill (ULV–HV), chainsaw (LV), wrench
- [x] Battery Buffer block registration (LV, MV, HV)
- [x] EnergyStorage в meta предмета (item может хранить EU)
- [x] ToolAction FlatBuffers протокол
- [x] ActionDispatcher обработчик ToolAction
- [x] Mining speed = f(tier, block_hardness)
- [x] CAS block break с потреблением энергии инструмента
- [x] Battery Buffer tick (20 Hz) — зарядка предметов
- [x] Client: слот для drill в BatteryBuffer GUI
- [x] Client: отображение заряда в тултипе предмета

---

## Раздел B: Wrench & Machine Side Config

### Что нужно сделать

Wrench для переназначения сторон машины (какая грань — INPUT, OUTPUT, ENERGY, FLUID, NONE).

### Где смотреть

| Файл | Роль |
|------|------|
| `src/protocol/core.fbs` | MachineAction — SET_SIDE_CONFIG |
| `src/services/simulation_core/ECS/Components/MachineComponent.h` | side_config — уже есть или нужно добавить поле |
| `src/services/simulation_core/main.cpp` | Обработка machine.action топика |
| `src/services/entity_state_store/` | Сохранение конфигурации сторон |
| `src/services/game_client/` | MachineWindow — side config UI |

### Архитектура

**6 граней (GTNH convention):**
- DOWN (0), UP (1), NORTH (2), SOUTH (3), WEST (4), EAST (5)

**Роли граней:**
- INPUT — предметы внутрь
- OUTPUT — предметы наружу
- FLUID_INPUT — жидкость внутрь
- FLUID_OUTPUT — жидкость наружу
- ENERGY — кабель/энергия
- ANY — авто (дефолт)
- NONE — заблокировано

**Wrench flow:**
1. ПКМ ключом по грани машины
2. Client определяет грань через raycast
3. Отправляет MachineAction(WRENCH_CYCLE, pos, face)
4. Сервер циклически меняет роль: INPUT → OUTPUT → ENERGY → FLUID_IN → FLUID_OUT → INPUT
5. Client обновляет текстуру грани

**PipeNetwork routing:**
- PipeNetwork BFS учитывает roles граней
- INPUT — можно вставить предмет
- OUTPUT — можно забрать предмет
- ENERGY — энергетическое соединение
- NONE — игнорируется BFS

### Блоки

| Item/Block | ID | Назначение |
|------------|----|------------|
 | wrench | N/A | Переназначение сторон |

### Критерии готовности

- [x] side_config: array<uint8, 6> в MachineComponent (существует с 7 ролями)
- [x] ToolAction/ToolActionResp протокол (core.fbs: kToolAction=13, kToolActionResp=14)
- [x] Gateway: kToolAction=13 switch case → publish "player.tool.action"
- [x] SimulationCore: subscribe "player.tool.action" → WrenchHandler::cycleFace → publish response
- [x] Client: G key → SendToolAction(WRENCH_CYCLE) → ToolActionRespCallback
- [x] Циклическое переключение ролей на сервере (INPUT→OUTPUT→ENERGY→FLUID_IN→FLUID_OUT→ANY→NONE)
- [ ] Wrench tool: клиент определяет грань при ПКМ (raycast face detection) — **B3 pending**
- [ ] Сохранение side_config в EntityStateStore
- [ ] Publish machine.config.updated при изменении
- [ ] Client: обновление текстуры грани
- [ ] PipeNetwork: учёт side_config при BFS routing
