# EPIC: Machines & Multiblocks — L1 Completion

**Layer**: L1  
**Статус**: 🔴 Не начато (2 задачи)  
**Base**: `1-gameplay-machines-multiblocks` (существующий эпик)

## Сводка

После рефакторинга MachineSystem, HeatTransferSystem и интеграции с PipeNetwork осталось 2 L1 задачи для завершения машин:

1. **Machine slot interaction** — клиент → сервер: положить/забрать предметы в слоты машины
2. **Server machine registration** — все машины получают ECS entity при установке

---

## Задача 1: Machine Slot Interaction

### Текущее состояние: почти готово (из кода)

Анализ кода показал, что machine slot interaction **уже реализован**, только без финального ACK:

**Есть в протоколе:**
- ✅ `SetMachineSlotReq` в `core.fbs` (lines 385-398) — player_id, pos, slot_idx, item_id, count, action
- ⚠️ **Нет `SetMachineSlotResp`** — клиент не получает явного подтверждения

**Есть в Gateway:**
- ✅ `GatewayMsg.kSetMachineSlot = 15` в `gateway.h` / `NetClient.h`
- ✅ Gateway.cpp (lines 358-363): публикует SetMachineSlotReq в `player.machine.slot` topic

**Есть в SimulationCore:**
- ✅ `main.cpp` (lines 382-436): полный handler для `player.machine.slot`
  - Верифицирует SetMachineSlotReq
  - Находит machine entity по позиции
  - Обновляет InventoryContainer слот
  - Сохраняет в EntityStateStore
  - Публикует BlockEntityUpdate → клиент

**Есть в клиенте:**
- ✅ `MachineWindow.cpp` вызывает `SendSetMachineSlot()` для PUT и TAKE

**Flow сейчас:**
```
Client → Gateway → "player.machine.slot" → SimulationCore → ESS → BlockEntityUpdate → Client
                                                                                ↳ нет SetMachineSlotResp
```

### Что осталось сделать

Единственный gap: **добавить `SetMachineSlotResp`** для явного ACK.

Фикс:
1. Добавить `table SetMachineSlotResp` в `core.fbs`:
```flatbuffers
table SetMachineSlotResp {
    pos: Vec3i;
    slot_idx: uint8;
    success: bool;
    error: string;       // "inventory_full", "no_permission", etc.
    item: ItemStack;     // текущий предмет в слоте после операции
}
```
2. SimulationCore handler: перед `break` (после обработки) отправлять `SetMachineSlotResp` через Gateway → Client
3. Клиент: обрабатывать `SetMachineSlotResp` для UI фидбека

### Критерии готовности

- [ ] `SetMachineSlotResp` добавлен в `core.fbs`
- [ ] SimulationCore handler шлёт SetMachineSlotResp после обработки
- [ ] Клиент обрабатывает SetMachineSlotResp (подтверждение/ошибка)
- [ ] Пересохранение через EntityStateStore (уже работает)

---

## Задача 2: Server Machine Registration

### Что нужно сделать

При установке любого блока-машины (SetBlockAction → CAS confirm) SimulationCore должен создавать ECS entity с MachineComponent, Position, EnergyStorage, InventoryContainer, RecipeProgress.

### Где смотреть

| Файл | Роль |
|------|------|
| `src/services/simulation_core/main.cpp` | `onBlockChanged` — текущая логика создания entity |
| `src/services/simulation_core/ECS/SimulationEngine.cpp` | `onBlockChanged` — какие block_id проверяет |
| `src/data/registry/consumers.csv` | Список machine block_id (furnace=36, macerator=48...) |
| `src/data/registry/producers.csv` | Список generator block_id (heat_generator=46...) |
| `src/services/simulation_core/EventListener/ChunkEventHandler.h/.cpp` | Конвертация BlockChangedEvent → ECS entity |
| `src/services/simulation_core/ECS/Components/MachineComponent.h` | Поля: machine_type, energy_in, energy_out |
| `src/services/simulation_core/ECS/Components/EnergyStorage.h` | Capacity, current, maxInput/maxOutput, tier |
| `src/services/simulation_core/ECS/Components/InventoryContainer.h` | 9-slot inventory |
| `src/services/simulation_core/Actions/SetBlockCASHandler.cpp` | CAS → callback → регистрация |

### Текущее состояние (из анализа кода)

**Зарегистрировано 13 машин** в MachineRegistry (consumers.csv + producers.csv):

| ID | Название | Роль | Энергия | Слоты in/out |
|----|----------|------|---------|-------------|
| 36 | heat_furnace | CONSUMER | HEAT | 1/1 |
| 46 | heat_generator | PRODUCER | HEAT | 0 |
| 48 | heat_macerator | CONSUMER | HEAT | 1/1 |
| 49 | steam_solid_boiler | PRODUCER | STEAM | 2/1 |
| 50 | steam_heat_boiler | PRODUCER | STEAM/HEAT | 1/1 |
| 51 | steam_macerator | CONSUMER | STEAM | 1/1 |
| 52 | steam_compressor | CONSUMER | STEAM | 1/1 |
| 60 | bronze_alloy_smelter | CONSUMER | STEAM | 2/1 |
| 61 | steam_extractor | CONSUMER | STEAM | 1/1 |
| 62 | steam_mixer | CONSUMER | STEAM | 2/1 |
| 63 | creative_generator | PRODUCER | ELECTRICITY | 10 |

**ECS entity creation в `SimulationEngine::onBlockChanged`:**
- Для registered машин: создаёт MachineComponent + RecipeProgress + InventoryContainer + EnergyStorage
- EnergyType из MachineRegistry, слоты из `defaultMachineSlotCount()`
- `isMachineBlock()` — **хардкоженная** проверка (не только по MachineRegistry)

### Найденные проблемы (gaps)

1. **Unregistered блоки получают entity** — `isMachineBlock()` имеет hardcoded fallback для block_id 14(crafting_table), 35(iron_pickaxe), 37(chest), 41-43(workbench_tools). Это НЕ машины — не должны получать MachineComponent/EnergyStorage/RecipeProgress.

2. **defaultMachineSlotCount()** — хардкод вместо чтения из MachineRegistry. 13 машин уже имеют правильное количество слотов в CSV, но код их не читает.

3. **Нет валидации** — при установке блока нет проверки "этот block_id — машина?" через единый registry lookup.

4. **EnergyStorage не для всех** — registered машин получают правильный EnergyType. Fallback-блоки получают ELECTRICITY по умолчанию.

### Что нужно сделать

**A. Очистить isMachineBlock()** — убрать хардкод, использовать только MachineRegistry::getMachineInfo():
```cpp
// Текущий хардкод (надо убрать):
bool isMachineBlock(uint16_t id) {
    return id == 36 || id == 48 || id == 52 || id == 14 || id == 35 || ...;
}
// Должно быть:
bool isMachineBlock(uint16_t id) {
    return MachineRegistry::getMachineInfo(id) != nullptr;
}
```

**B. defaultMachineSlotCount() → из MachineRegistry:**
```cpp
// Вместо хардкода:
int slots = MachineRegistry::getMachineInfo(block_id).slots_in + 
            MachineRegistry::getMachineInfo(block_id).slots_out;
```

**C. EnergyStorage инициализация:**
- Читать capacity/tier/maxInput/maxOutput из MachineRegistry (добавить поля в CSV если нет)
- Fallback: capacity=10000, tier=0, maxInput=32, maxOutput=32

**D. Добавить missing машины в MachineRegistry:**
- Проверить все block_id из items.csv, которые ДОЛЖНЫ быть машинами
- Добавить их в consumers.csv/producers.csv с корректными параметрами

### Критерии готовности

- [ ] `isMachineBlock()` → MachineRegistry::getMachineInfo() lookup, без хардкода
- [ ] `defaultMachineSlotCount()` → из MachineRegistry (slots_in + slots_out)
- [ ] EnergyStorage инициализируется из MachineRegistry (capacity, tier, maxInput/maxOutput)
- [ ] InventoryContainer создаётся с правильным количеством слотов из реестра
- [ ] RecipeProgress = 0 (idle)
- [ ] Валидация: только registered machine block_id получают MachineComponent
- [ ] Non-machine блоки (crafting_table, tools) НЕ получают MachineComponent
- [ ] Entity сохраняется через EntityStateStore при создании
- [ ] При удалении блока — entity удаляется из ECS и ESS
