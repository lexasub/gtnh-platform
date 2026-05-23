# Инфраструктура, автоматизация и MVP

**Эпик**: Infrastructure, Automation & MVP
**Слой**: Layer 3 (Automation) — отложено | Cross-cutting — сейчас
**Статус**: Mixed (Infrastructure — в работе, Automation — отложено)

## Affected Services

| Service | Layer | Role |
|---------|-------|------|
| **MessageRouter** (Go) | L0 | Central pub/sub — every service connects here |
| **RecipeManager** ⬅️ **NEW** | L0 | Recipe loading, RPC |
| **EntityStateStore** ⬅️ **NEW** | L0 | TileEntity persistence |
| **MetaDB** (Go/SQLite) | L0 | Player data |
| **SimulationCore** | L1 | Tick, machines, multiblocks |
| **GameClient** | L1 | All ImGui windows |
| **WorldGenerator** | L0 | Library linked into ChunkStore — procedural terrain + ore veins |
| **Gateway** | L0 | TCP relay |
| **ChunkStore** | L0 | Block storage |



---

## Раздел A: Инфраструктурные сервисы

### A.1 EntityStateStore

C++ сервис для хранения состояний TileEntity по координатам блока.

- **Бэкенд**: LMDB
- **Ключ**: `dim|x|y|z` → blob
- **RPC**: `GetState(key) → blob`, `SetState(key, blob)`
- **Используется для**: инвентари машин, верстаков, сундуков
- **Принцип**: отдельный от ChunkStore, не нарушает dumb storage архитектуру

### A.2 RecipeManager

C++ сервис для загрузки и проверки рецептов.

- **Хранение**: JSON-файлы в `data/recipes/`, загружаются в память при старте
- **RPC**: `CheckRecipe(container, machine_type) → recipe_id`
- **RPC**: `Craft(recipe_id, container) → new_container`
- **Клиенты**: SimulationCore (проверка крафтов, тики машин)

### A.3 MetaDB Integration

Go-сервис (SQLite) для данных игроков.

- **Уже скомпилирована**, нужно дописать сетевой слой для подключения к MessageRouter
- **Хранит**: PlayerInventory (blob), позиция игрока
- **Форвард через Gateway**: клиент → Gateway → MetaDB

---

## Раздел B: Протокол — дополнения

Полная спецификация протокола базовой механики описана в [0-basic-mechanics/basic-mechanics.md](../0-basic-mechanics/basic-mechanics.md) (секция 7 — FlatBuffers).

Сообщения, определённые там: `ItemStack`, `PlayerAction` (с `selected_slot`), `InventoryUpdate`, `CraftRequest`, `BlockEntityUpdate`.

Здесь перечислены только изменения/дополнения, специфичные для инфраструктуры:

```flatbuffers
// Дополнительное поле energy в BlockEntityUpdate
table BlockEntityUpdate {
    x: uint32; y: uint32; z: uint32;
    machine_type: uint16;
    progress: float;
    energy: uint32;          // NEW — energia (сетевая)
    inventory: [ItemStack];
}
```

---

## Раздел C: SimulationCore — расширения

### Новые возможности

- **Обработка контейнеров**: open/close для верстаков и машин
- **InventoryAction**: клиент шлёт действия с предметами (move, split, merge)
- **TileEntity tick system**: обобщённый механизм тиков для любых машин (20 Hz)
- **Логика простых машин**: жёстко прописать в коде (или через скрипты) для MVP: furnace, macerator, compressor

---

## Раздел D: Клиент — GUI

Спецификация UI компонент описана в [0-basic-mechanics/basic-mechanics.md](../0-basic-mechanics/basic-mechanics.md) (секция 5 — UI компоненты).

Все окна базовой механики перечислены там. Дополнительные замечания для инженеров:

- **Игрок не рендерится** — ни рука, ни тело. Только инвентарь и мир.
- **Все окна — ImGui**. Никакого кастомного рендера.
- **ui.md** — файл со списком всех UI компонентов и их состояний.

---

## Раздел E: WorldGenerator

### Генерация руд

- Руды как блоки нужны для машин (печка, дробилка)
- Генерация: простая синусоидальная жила (не сложная система жил GTNH)
- **Без инструментов**: любой предмет/рука ломает блок, предмет сразу в инвентарь

---

## Раздел F: Автоматизация и логистика (Layer 3 — отложено)

### F.1 Автоматический ввод/вывод (3.1)

**Что**: перемещение предметов между инвентарями (хопперы, трубы предметов).

**Архитектура**: компонент `ItemTransporter` в ECS. SimulationCore проверяет соседние инвентари, перемещает предметы согласно правилам (стороны, фильтры).

**Примитивы**: транспорт предметов — сейчас отсутствует.

### F.2 Логистические сети (3.2)

**Что**: AE2-подобная система запросов предметов, приоритеты, крафт по требованию.

**Отложено**. Архитектурная закладка: инвентари должны иметь уникальные идентификаторы в ECS для обращений по сети.

### F.3 Редстоун / управляющие сигналы (3.3)

**Решение**: НЕ ДЕЛАТЬ. Только если когда-нибудь игра станет вполне играбельной.

Блоки-источники сигнала изменяют meta блока. SimulationCore подписывается на изменения, передаёт в соседние машины.

---

## Раздел G: MVP — план внедрения

Базовые правила MVP описаны в [0-basic-mechanics/basic-mechanics.md](../0-basic-mechanics/basic-mechanics.md) (секция 6).

### Порядок реализации

1. **EntityStateStore** (C++) — LMDB-хранилище по координатам
2. **RecipeManager** (C++) — загрузка JSON, RPC
3. **MetaDB** — сетевой слой для роутера
4. **Протокол** — см. basic-mechanics.md секция 7
5. **SimulationCore** — контейнеры, инвентарь, тики машин
6. **Клиент** — ImGui окна (см. basic-mechanics.md секция 5)
7. **WorldGenerator** — руды (простая синусоида)

### Layer 2 (отложено)

После простых машин и крафтов:
- Генераторы и передача энергии (сначала через соседние блоки, без графа)
- Мультиблоки
- Провода, трубы

---

## Открытые вопросы

1. **EntityStateStore** — делать отдельный C++ сервис или часть ChunkStore?
2. **SimulationCore tick** — как маршрутизировать событие `BlockChanged` от ChunkStore?
3. **ImGui синхронизация** — частота обновления GUI машин (каждый тик? раз в секунду?)
4. **WorldGenerator руды** — какие именно руды генерировать для тестирования машин?
