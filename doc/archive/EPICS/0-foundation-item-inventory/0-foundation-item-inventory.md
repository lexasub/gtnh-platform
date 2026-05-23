# Система предметов и инвентарей (Item & Inventory)

**Эпик**: Foundation — Item & Inventory
**Слой**: Layer 0 (Primitives)
**Статус**: Draft

## Affected Services

| Service | Layer | Role |
|---------|-------|------|
| **ChunkStore** | L0 | NOT involved — block data never stores inventories |
| **Protocol** (`.fbs`) | L0 | Shared — ItemStack, InventorySlot, InventoryUpdate in `core.fbs` |
| **MetaDB** (Go/SQLite) | L0 | Primary for player inventory + **MVP host for EntityStateStore** |
| **tile_entity_store.fbs** ⬅️ **NEW** | L0 | Schema for machine/workbench/chest state persistence |
| **SimulationCore** | L1 | Post-hook — reacts to inventory events for gameplay |
| **Gateway** | L0 | Relay — forwards inventory data between services and client |
| **GameClient** | L1 | Consumer — renders inventory GUI |

> **✅ Реализовано**: `entity_state_store` — отдельный C++ сервис с LMDB. TCP RPC порт 5200 + MessageRouter pub/sub (`entity.state.get/set`).



---

## Обзор

Фундаментальные примитивы для работы с предметами и их хранением. ItemStack — базовый тип данных для всех сервисов. Inventory — контейнер ItemStack'ов с фиксированным или динамическим числом слотов.

---

## Item Stack

### Описание

Абстрактный ItemStack = item_id + count + meta (прочность/состояние/damage). Без NBT на MVP.

### Где живёт

Общий тип данных для всех сервисов, описан в FlatBuffers schema (`protocol/core.fbs`).

### Структура

```flatbuffers
table ItemStack {
    item_id: uint16;    // ID предмета/блока
    count: uint8;       // количество (1–64/128, зависит от предмета)
    meta: uint16;       // прочность/состояние/damage (0 = новая)
}
```

### Зачем

Инвентари, рецепты, дропы, машины — всё оперирует ItemStack. Это единственный способ представления предмета в системе.

---

## Inventory

### Описание

Контейнер с фиксированным количеством слотов (может быть динамическим позже). Каждый слот содержит ItemStack или пуст (item_id = 0).

### Типы инвентарей

| Тип | Размер | Где хранится состояние |
|-----|--------|----------------------|
| Игрок | 36 + 9 хотбар | MetaDB (сериализованный blob) |
| Машина | input/output (зависит от машины) | EntityStateStore |
| Верстак | 3×3 + 1 результат | EntityStateStore |
| Сундук (позже) | 9×N | EntityStateStore |

### Архитектура хранения

**Правило разделения:**
- **MetaDB** — данные, привязанные к игроку (инвентарь игрока, экипировка)
- **EntityStateStore** — данные, привязанные к миру (состояние машин, верстаков, сундуков)
- **ChunkStore** — НЕ хранит инвентари. Только блоки и meta-слой.

**Почему не в chunk meta-слое?**
Чанк — неизменяемый блок-дамп. Состояние машины/верстака — это TileEntity, которое часто меняется. Хранить его нужно в сервисе, оптимизированном под частые изменения и запросы по координатам.

### EntityStateStore ✅ (C++ service, реализован)

Отдельный C++ сервис (`src/services/entity_state_store/`). 11 файлов, CMakeLists.txt собирает `entitystated`.

- **Бэкенд**: LMDB
- **Ключ**: `(dim, x, y, z)` → blob
- **Схемы**: `entity_state_store.fbs` (GetEntityStateReq/Resp, SetEntityStateReq/Ack), `tile_entity_store.fbs` (machine state)
- **RPC**: FlatBuffers TCP (`:5200`) + MessageRouter pub/sub (`entity.state.get`, `entity.state.set`)
- **Клиенты**: MessageRouterClient + ChunkStoreClient (подписан на `world.blocks.changed`)
- **main.cpp**: 250 строк, полная обработка запросов

### PlayerInventory

Хранится в MetaDB как сериализованный blob (или отдельная таблица). Логин/логаут флоу описан в [0-basic-mechanics/basic-mechanics.md](../0-basic-mechanics/basic-mechanics.md) (секция 1 — Инвентарь игрока).

### Protocol (flatbuffers)

Полная спецификация протокола — в [0-basic-mechanics/basic-mechanics.md](../0-basic-mechanics/basic-mechanics.md) (секция 7).

Базовый тип данных (shared across all services):
```flatbuffers
table ItemStack {
    item_id: uint16;
    count: uint8;
    meta: uint16;
}
```

---

## Осталось реализовать (перенесено из archive/1-player-crafting)

- [ ] **Server-authoritative grid state через EntityStateStore** — `WorkbenchStateManager` хранит грид в памяти, не подключён к EntityStateStore RPC. При закрытии/открытии верстака грид теряется.
- [ ] **Полный inventory consumption через MetaDB** — `CraftRequestHandler::publishInventoryUpdate()` создаёт пустой `InventoryUpdate` без consumption delta. Реальный расход ингредиентов из инвентаря игрока не публикуется в MetaDB.

## Открытые вопросы

1. **Blob-формат** — FlatBuffers или простой бинарный дамп для хранения в MetaDB/EntityStateStore?
2. **Инвентарь сундуков** — в EntityStateStore или всё же в ChunkStore как часть блока?
3. **Лимит стаков** — использовать стандартный Minecraft stack size (64) или GTNH-кастомный?
4. **NBT позже** — закладывать ли тег-контейнер в ItemStack для будущих расширений?
