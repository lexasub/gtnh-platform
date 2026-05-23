# TASK: Протокол — сообщения базовой механики

**Layer**: 0–1
**Статус**: Draft
**Эпик**: 0-basic-mechanics

## Affected Services

| Service | Role |
|---------|------|
| **Protocol** | FlatBuffers schema — все сообщения в `core.fbs` / `player_action.fbs` |
| **Gateway** | Relay — forwards все сообщения |
| **SimulationCore** | Consumer (PlayerAction), Producer (InventoryUpdate, BlockEntityUpdate) |
| **GameClient** | Consumer (InventoryUpdate, BlockEntityUpdate), Producer (PlayerAction, CraftRequest) |
| **MetaDB** | Consumer (InventoryUpdate — сохранение) |

---

## Описание

Набор FlatBuffers сообщений, необходимых для реализации базовой механики игрока (инвентарь, блоки, верстак).

---

## FlatBuffers Schema

### ItemStack (базовый тип данных)
```flatbuffers
table ItemStack {
    item_id: uint16;   // ID предмета/блока (0 = пусто)
    count: uint8;      // количество в стаке (1–64)
    meta: uint16;      // прочность/состояние (0 = новая)
}
```

**Правила:**
- `item_id = 0` = пустой слот (empty stack convention)
- Имя предмета НЕ передаётся по сети — только uint16
- Display-информация — из ItemRegistry по запросу клиента

### PlayerAction (действие игрока)
```flatbuffers
enum PlayerActionType : uint8 {
    PLACE = 0,     // поставить блок
    BREAK = 1,     // сломать блок
    MOVE  = 2,     // переместить предмет
    USE   = 3,     // использовать предмет
}

table PlayerAction {
    player_id: uint64;         // ID игрока (MVP: 0)
    action: PlayerActionType;  // тип действия
    x: uint32;                 // координаты
    y: uint32;
    z: uint32;
    block_id: uint16;          // ID блока
    selected_slot: uint8;      // выбранный слот хотбара (0–9)
}
```

### InventoryUpdate (обновление инвентаря)
```flatbuffers
table InventoryUpdate {
    player_id: uint64;
    slots: [ItemStack];        // полный снимок инвентаря (45 слотов)
}

// В будущем: delta-encoding вместо полного снепшота
```

**Назначение:** Сервер → Клиент. Полный снимок инвентаря игрока при каждом изменении.

### CraftRequest (запрос крафта — stub)
```flatbuffers
table CraftRequest {
    player_id: uint64;
    x: uint32; y: uint32; z: uint32;    // позиция верстака
    slots: [ItemStack];                  // содержимое сетки 3×3 (9 слотов)
}
```

**Назначение:** Клиент → Сервер. Пока крафт не выполняется — сообщение-заглушка.

### BlockEntityUpdate (состояние машины/верстака)
```flatbuffers
table BlockEntityUpdate {
    x: uint32; y: uint32; z: uint32;
    machine_type: uint16;                // CRAFTING_TABLE=100, FURNACE=101, etc.
    inventory: [ItemStack];              // слоты машины
    progress: float;                     // прогресс обработки (0.0–1.0)
}
```

**Назначение:** Сервер → Клиент. Синхронизация состояния машины/верстака.

---

## Таблица сообщений

| Сообщение | Назначение | Направление | Частота |
|-----------|-----------|-------------|---------|
| `PlayerAction` | Игрок ставит/ломает блок, перемещает предмет | Client → Gateway → SimulationCore | По действию игрока |
| `InventoryUpdate` | Обновление инвентаря игрока (полный снимок) | SimulationCore → Gateway → Client | При каждом изменении |
| `CraftRequest` | Запрос крафта (stub) | Client → Gateway → SimulationCore | По нажатию Craft |
| `BlockEntityUpdate` | Состояние машины/верстака | SimulationCore → Gateway → Client | При изменении / каждый тик |

---

## Расположение файлов

| Файл | Содержимое |
|------|-----------|
| `src/protocol/core.fbs` | ItemStack, InventoryUpdate |
| `src/protocol/player_action.fbs` | PlayerAction, PlayerActionType |
| `src/protocol/craft_request.fbs` | CraftRequest |
| `src/protocol/block_entity_update.fbs` | BlockEntityUpdate |

---

## Acceptance Criteria

#### Сценарий: PlayerAction с выбранным слотом
1. Игрок выбирает слот 3 хотбара
2. Клиент шлёт `PlayerAction{action: PLACE, x, y, z, block_id: 3, selected_slot: 3}`
3. FlatBuffers schema компилируется без ошибок
4. `selected_slot` ∈ 0–9

#### Сценарий: InventoryUpdate полный снимок
1. Инвентарь игрока изменился
2. SimulationCore шлёт `InventoryUpdate` с 45 `ItemStack`
3. Клиент парсит сообщение, обновляет UI
4. Все пустые слоты: `item_id = 0`

#### Сценарий: CraftRequest (stub)
1. Игрок нажимает Craft в верстаке
2. Клиент шлёт `CraftRequest` с текущим содержимым сетки
3. Сервер получает сообщение
4. Ничего не происходит (ответ не требуется)
