# Базовая механика игрока (Basic Mechanics — MVP)

**Эпик**: Basic Mechanics — MVP
**Слой**: Layer 1 (Player Mechanics)
**Статус**: Draft

## Affected Services

| Service | Layer | Role |
|---------|-------|------|
| **MetaDB** (Go/SQLite) | L0 | Primary — player inventory persistence |
| **EntityStateStore** ⬅️ **MVP** | L0 | Persistence — workbench/machine TileEntity state (внутри MetaDB) |
| **Gateway** | L0 | Relay — forwards PlayerAction, InventoryUpdate |
| **SimulationCore** | L1 | Validates item placement/consumption |
| **GameClient** | L1 | Consumer — inventory GUI, hotbar, workbench, creative menu, machines |

> **Архитектурное правило**: Инвентарём владеет MetaDB. SimulationCore не управляет состоянием инвентаря — только подписывается на события для геймплейной логики.
>
> **Архитектурное правило (UI)**: GameClient НЕ знает конкретных типов окон. Взаимодействие — только через UIManager (Mediator) и UIDefaults. Добавление нового окна не меняет GameClient.
>
> **Архитектурное правило (MachineWindow)**: MachineWindow не хранит слоты/прогресс/энергию как поля. Все данные читаются из IMechanism*, который может быть локальным моком или сетевым прокси.
>
> **Идентификация**: На MVP один пользователь с `user_id = 0`. Система не отслеживает нескольких пользователей.

---

## 1. Инвентарь игрока

### Хотбар
- Полоска из 10 слотов внизу экрана
- Выбор слота клавишами 1–9 (0 для 10-го)

### Полный инвентарь
- Клавиша **E** открывает окно инвентаря: 4 строки × 10 слотов
- Слоты хотбара отображаются в нижней части окна
- Выход из инвентаря: **Escape** или повторное **E**

### Ограничения
- Области крафта в интерфейсе игрока **НЕТ** — крафт только через верстак
- Реализация инвентаря должна быть подменяема (возможно увеличение размера в будущем)
- Не рисуем руки пользователя и его самого (только UI)

### Персистентность
- Инвентарь хранится в MetaDB как сериализованный blob
- Загружается при "логине" (подключении)
- Сохраняется при "логауте" (отключении)
- На MVP: один пользователь `user_id = 0`

### Формат данных (ItemStack)
```
ItemStack {
    item_id: uint16;   // ID предмета/блока (0 = пусто)
    count: uint8;      // количество (1–64)
    meta: uint16;      // прочность/состояние (0 = новая)
}
```

---

## 2. Взаимодействие с блоками

### Установка блоков
- Пользователь ставит блок из выбранного слота хотбара
- ЛКМ по миру ставит блок
- Блок расходуется из инвентаря

### Ломка блоков
- Зажать ЛКМ на блоке → блок разрушается
- Сломанный блок сразу попадает в инвентарь (без анимации дропа)
- Нет выпавших предметов на землю

### Упрощения MVP
- **Нет инструментов**: любой предмет/рука ломает блок с одинаковой скоростью
- **Нет прочности**: предметы не расходуются (кроме блоков при постройке)
- **Полное доверие клиенту**: клиент сообщает действие, сервер не проверяет

---

## 3. Машины и верстак

### Верстак (первая машина)
- Первая машина, доступная игроку — **верстак** (BlockType::CraftingTable)
- Пользователь ставит блок верстака, ПКМ открывает GUI

### GUI верстака
- Сетка крафта **3×3** (9 слотов)
- Слот результата (справа)
- ✅ **Крафт работает**: клик "Craft" → CraftRequest → Gateway → CraftRequestHandler → GridPatternMatcher + RecipeManager → CraftResponse
- ✅ **MatchPattern() preview**: 8 встроенных паттернов, превью обновляется при изменении сетки

### MachineWindow (Furnace, Macerator, Compressor)
- Data-driven: все данные (слоты, прогресс, энергия) читаются из `IMechanism*`
- Количество слотов не захардкожено — рисуется столько, сколько вернул вектор
- `Tick(dt)` вызывается из GameClient::Update для локальных моков
- `SetMechanism(unique_ptr<IMechanism>)` назначает источник данных

### Взаимодействие с инвентарём
- Пользователь может переносить предметы из своего инвентаря в слоты машин и обратно
- Перемещение: drag-and-drop или клик (MVP — упрощённо: `ClickCallback`)

### Персистентность
- Состояние сетки верстака хранится в **EntityStateStore** (на MVP — внутри MetaDB)
- Ключ: координаты блока `(dim, x, y, z)` → blob
- При повторном открытии содержимое восстанавливается

---

## 4. Креативное меню

### Назначение
- Без инструментов и рудодобычи игроку нужен способ получать предметы для тестирования крафтов и машин

### Решение (MVP)
- ImGui-окно со списком **всех зарегистрированных предметов/блоков**
- Игрок выбирает предмет и количество
- Предмет появляется в инвентаре игрока

### Варианты (не выбраны)
- ❌ Начальный сундук — негибко
- ❌ Бесконечный генератор — избыточно

Креативное меню — наименее трудозатратно, покрывает все механики.

---

## 5. UI компоненты

Все UI компоненты вынесены в отдельный файл **`ui.md`** (актуальная архитектура).

**Архитектура UI**: 4 паттерна — Mediator (UIManager), Strategy (IUIWindow), Factory (BlockUIFactory), Observer (network dispatch).

**Ключевое правило**: GameClient не включает хедеры конкретных окон. `UIDefaults.cpp` — единственное место с `#include "UI/Windows/*.h"`. Добавить новое окно = одна регистрация в UIDefaults + одна factory lambda. GameClient/UIManager не меняются.

**Все детали — в [`ui.md`](ui.md).**

### Список окон (текущий)
| Окно | Класс | Паттерн | Статус |
|------|-------|---------|--------|
| Инвентарь игрока | `InventoryWindow` | IUIWindow (Strategy), E-key | ✅ Built |
| Креативное меню | `CreativeMenu` | IUIWindow (Strategy), Tab-key | ✅ Built |
| Верстак | `WorkbenchWindow` | BlockAttachedWindow, 3×3 grid | ✅ Built |
| Машина | `MachineWindow` | BlockAttachedWindow + IMechanism* | ✅ Built |

---

## 6. Базовые правила MVP

| Правило | Описание | Обоснование |
|---------|----------|-------------|
| Нет дропа | Сломал блок → предмет сразу в инвентарь | Не нужна физика дропа, сбор предметов |
| Нет инструментов | Любой предмет/рука ломает блок одинаково | Не нужна система прочности, типы инструментов |
| Нет прочности | Предметы не расходуются (кроме блоков при постройке) | Не нужна система耐久 |
| Полное доверие | Клиент сообщает действие, сервер не проверяет | Античит отложен |
| Весь GUI — ImGui | Без кастомного рендера | ImGui прототипирование |
| Энергия — число | Машины работают "на магии" (бесконечная энергия) | Энергосети отложены |
| Крафт | Только через блок верстака | Нет крафта в инвентаре |
| Нет руки/тела | Не рендерим игрока | Упрощение рендеринга |
| **GameClient не знает типы окон** | Только UIDefaults + UIManager | Добавление окна не меняет GameClient |
| **UIDefaults — sole include point** | Единственный .cpp с хедерами конкретных окон | 1 registration + 1 lambda = новое окно |
| **FindOrCreate лениво** | Блок-окна создаются при первом ПКМ | Не занимают память, не нужно чистить |
| **MachineWindow data-driven** | Слоты из IMechanism, не хардкод | Любая машина с любым числом слотов |

---

## 7. Протокол (FlatBuffers)

### ItemStack (базовый тип)
```flatbuffers
table ItemStack {
    item_id: uint16;
    count: uint8;
    meta: uint16;
}
```

### PlayerAction (с новым полем selected_slot)
```flatbuffers
table PlayerAction {
    player_id: uint64;
    action: PlayerActionType;  // PLACE, BREAK, MOVE, USE
    x: uint32; y: uint32; z: uint32;
    block_id: uint16;
    selected_slot: uint8;      // ← новое поле для выбора слота
}
```

### InventoryUpdate (полный снимок инвентаря)
```flatbuffers
table InventoryUpdate {
    player_id: uint64;
    slots: [ItemStack];
}
```

### CraftRequest
```flatbuffers
table CraftRequest {
    player_id: uint64;
    x: uint32; y: uint32; z: uint32;
    slots: [ItemStack];
}
```
✅ **Реализован**: CraftRequest → Gateway(type 9) → SimulationCore(CraftRequestHandler) → GridPatternMatcher + RecipeManager → CraftResponse(type 10)

### BlockEntityUpdate (состояние машины/верстака)
```flatbuffers
table BlockEntityUpdate {
    x: uint32; y: uint32; z: uint32;
    machine_type: uint16;
    inventory: [ItemStack];
    progress: float;
}
```

### Сообщения по назначению
| Сообщение | Назначение | Откуда → Куда |
|-----------|-----------|---------------|
| `PlayerAction` | Действие игрока (поставить/сломать) | Client → Gateway → SimulationCore |
| `InventoryUpdate` | Обновление инвентаря | SimulationCore → Gateway → Client |
| `CraftRequest` | Запрос крафта | Client(type 9) → Gateway → SimulationCore(CraftRequestHandler) |
| `CraftResponse` | Результат крафта | SimulationCore → Gateway → Client(type 10) |
| `InventoryAction` | Действие с инвентарём | Client(type 7) → Gateway |
| `BlockEntityUpdate` | Состояние машины/верстака | SimulationCore → Gateway → Client(type 8) |
| `BlockAck` | Подтверждение блока | Gateway → Client(type 5) |

---

## 8. Remaining Work (что нужно доработать)

Полный список — в [`ui.md`](ui.md#8-remaining-work-что-нужно-реализовать).

**Кратко:**
| # | Задача | Приоритет | Статус |
|---|--------|-----------|--------|
| 1 | IMechanism interface | Medium | ✅ Done |
| 2 | Mock моки машин (Furnace/Macerator/Compressor) | Medium | ✅ Done |
| 3 | BlockType enum + consumer refactor | Low | ⬅️ In progress |
| 4 | ISidePanel система + стаб RecipePanel | Future | ⬅️ In progress |
| 5 | SlotGrid drag-and-drop (ClickCallback) | Medium | 🟡 ClickCallback done, needs state machine |
| 6 | WorkbenchWindow Render() refactor | Low | ⬅️ Todo (CraftResponse display) |
| 7 | NetClient wire-up | When API ready | ✅ **Done** (1-10, crafting + inventory) |
| 8 | UIManager/ProcessInput cleanup | Low | ⬅️ Todo |
| 9 | BlockUIFactory::All() рефакторинг | Low | ⬅️ Todo |

---

## 9. Открытые вопросы

1. **Ограничение стаков** — использовать стандартный Minecraft stack size (64) или GTNH-кастомный?
2. **Дроп блоков** — какие блоки дают предмет при ломке (все или некоторые)?
3. **Формат хранения blob'а** — FlatBuffers или простой бинарный дамп для MetaDB/EntityStateStore?
4. **Креативное меню** — показывать все item_id или только "разрешённые" для MVP?
5. ~~**CraftRequest** — проверять ли рецепты на клиенте?~~ ✅ **Решено**: полный pipeline с серверной валидацией через CraftRequestHandler + RecipeManager

---

## Связанные файлы

- `tasks/1-player-inventory.md` — задача на реализацию инвентаря
- `tasks/2-block-interaction.md` — задача на установку/ломку блоков
- `tasks/3-workbench.md` — задача на верстак
- `tasks/4-creative-menu.md` — задача на креативное меню
- `tasks/5-basic-rules.md` — задача на базовые правила
- `tasks/6-protocol.md` — задача на протокол
- `tasks/7-mechanism-window.md` — ⬅️ DEPRECATED (MachineWindow теперь напрямую extends BlockAttachedWindow + IMechanism)
- `tasks/8-serverlogic-mocks.md` — ⬅️ IMechanism + mock machines
- `tasks/9-block-type-enum.md` — ⬅️ BlockType consolidation (in progress)
- `tasks/10-slot-drag-and-drop.md` — ⬅️ SlotGrid drag-and-drop
- `tasks/11-netcient-wire-up.md` — ⬅️ NetClient integration (blocked)
