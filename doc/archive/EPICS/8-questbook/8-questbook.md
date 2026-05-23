# EPIC: Quest Book

**Layer**: L0  
**Статус**: 🔴 **Not started**  
**Последнее обновление**: 2026-06-28  
**Зависимости**: Inventory system, CraftRequestHandler, MachineSystem, EntityStateStore, MetaDB (SQLite)

## Userflow диаграммы

- `doc/userflow/01-player-interaction.puml` — P2: Крафт, P3: Инвентарь
- `doc/userflow/02-autonomous-operation.puml` — A1: Симуляция без игрока
- `doc/userflow/04-machine-operations.puml` — M1: Machine Lifecycle

## Обзор

Quest Book — аналог квестовой книги из GTNH. Направляет игрока: показывает что скрафтить, что построить, в каком порядке. Игрок отмечает прогресс (автоматически или вручную), открываются следующие квесты.

**Что НЕ будет:**
- Никаких команд/гильдий
- Никаких оценок/лидербордов
- Никакого соревновательного мультиплеера

**Что будет:**
- Эпохи (Vagrant → Apprentice → Expert → Administrator)
- Секции внутри эпох (Foundation, Electric Tools, Machine Config, Transport...)
- Квесты с требованиями (скрафтить, поставить блок, зарядить инструмент, настроить грань)
- Автоматическое и ручное завершение
- Разблокировка следующих нод при завершении

---

## Data Format

### Принцип

CSV — данные, которые правим руками и читаем глазами. JSON — склейка по ID.

### `quests.csv` — реестр квестов

```csv
id,era_id,section_id,title,description,icon_item_id,manual_complete
q001,0,foundation,"Welcome to GTNH","Welcome to the Foundation era. Craft your first blocks.","1",0
q002,0,foundation,"Crafting Table","Craft a crafting table to unlock basic recipes.","58",0
q003,0,foundation,"First Pickaxe","Craft a wooden pickaxe. Time to mine!","278",0
q010,1,electric_tools,"Craft Wrench","Craft a wrench. Used to configure machine sides.","95",0
q011,1,electric_tools,"Craft ULV Drill","Craft your first electric drill. Mines stone, gravel, coal.","90",0
q012,1,electric_tools,"Charge Your Drill","Charge the ULV drill in a Battery Buffer to 100 EU.","90",0
q013,1,electric_tools,"Craft LV Drill","Craft an LV drill. Mines iron, copper, tin.","91",0
q020,1,machine_config,"Place a Machine","Place any single-block machine (furnace, macerator, etc.).","36",0
q021,1,machine_config,"Configure Machine Side","Use wrench on a machine face to change its role.","95",1
q022,1,machine_config,"Connect Energy","Place an LV cable next to a generator and a machine.","66",0
q030,1,transport,"Place Item Pipe","Place an item pipe between two inventories.","62",0
q031,1,transport,"Connect Machine to Pipe","Connect a machine output to a chest via item pipe.","62",0
q040,2,autonomous_mining,"Place Mining Drill","Place a mining drill block on ore.","98",0
q041,2,autonomous_mining,"Power the Drill","Connect energy to the mining drill.","66",0
q042,2,autonomous_mining,"Collect Output","Connect an item pipe to the drill output.","62",0
q050,2,processing,"Build Ore Processing Line","Build: Macerator → Furnace → Compressor connected by pipes.","36",1
q060,3,multiblocks,"Assemble EBF","Build an Electric Blast Furnace multiblock (3x3x4).","72",0
q061,3,multiblocks,"First EBF Recipe","Run a recipe in the EBF.","72",0
```

**Поля:**
| Поле | Тип | Описание |
|------|-----|----------|
| `id` | string | Уникальный ID квеста |
| `era_id` | int | Эпоха (0=Vagrant, 1=Apprentice, 2=Expert, 3=Administrator) |
| `section_id` | string | ID секции внутри эпохи |
| `title` | string | Название квеста (кратко) |
| `description` | string | Описание (что сделать, зачем) |
| `icon_item_id` | int | Item ID для иконки в UI |
| `manual_complete` | int | 0=авто, 1=только ручное завершение |

### `quest_deps.csv` — граф зависимостей

```csv
quest_id,depends_on,type
q002,q001,complete
q003,q002,complete
q010,q003,complete
q011,q010,complete
q012,q011,complete
q013,q012,complete
q020,q010,complete
q021,q020,complete
q022,q021,complete
q030,q022,complete
q031,q030,complete
q040,q013,complete
q040,q022,complete
q041,q040,complete
q042,q041,complete
q050,q031,complete
q050,q042,complete
q060,q050,complete
q061,q060,complete
```

**Поля:**
| Поле | Тип | Описание |
|------|-----|----------|
| `quest_id` | string | Квест-зависимый |
| `depends_on` | string | Квест от которого зависит |
| `type` | string | Тип зависимости: `complete` (надо завершить), `unlock` (надо разблокировать) |

**Правила:**
- Если у квеста 0 dependences → он разблокирован сразу (стартовый)
- Если dependences несколько → нужны ВСЕ (AND)
- OR-зависимости не поддерживаем (можно эмулировать через промежуточные квесты)

### `quest_reqs.csv` — требования для завершения

```csv
quest_id,req_type,req_id,req_count,label
q002,craft,58,1,"Craft a crafting table"
q003,craft,278,1,"Craft a pickaxe"
q010,craft,95,1,"Craft a wrench"
q011,craft,90,1,"Craft ULV drill"
q012,charge_tool,90,100,"Charge drill to 100 EU"
q013,craft,91,1,"Craft LV drill"
q020,place_block,36,1,"Place a furnace or macerator"
q021,wrench_cycle,0,1,"Cycle a machine face with wrench"
q022,place_block,66,1,"Place LV cable next to generator"
q030,place_block,62,1,"Place item pipe"
q031,pipe_flow,0,1,"Item flows through pipe into chest"
q040,place_block,98,1,"Place mining drill"
q041,energy_flow,0,128,"Energy flows to drill (128+ EU/t)"
q042,pipe_flow,0,1,"Item flows from drill to chest"
q050,chain_flow,0,3,"3+ machines connected in processing chain"
q060,multiblock_valid,72,1,"EBF structure is valid"
q061,recipe_complete,72,1,"EBF completes one recipe"
```

**req_type — типы проверок:**

| Тип | Что проверяет | Откуда берём |
|-----|---------------|-------------|
| `craft` | Игрок скрафтил item_id count раз | CraftRequestHandler |
| `place_block` | Игрок поставил block_id | SetBlockAction / ChunkStore |
| `charge_tool` | Инструмент имеет заряд ≥ count | ItemEnergyStorage |
| `wrench_cycle` | Грань машины переключена | WrenchHandler::cycleFace |
| `pipe_flow` | Предмет прошёл через трубу | PipeNetwork::moveItemsInNetwork |
| `energy_flow` | Энергия передана (≥ count EU/t) | PipeNetwork::distributeEnergy |
| `chain_flow` | Цепочка из N+ машин соединена | PipeNetwork BFS |
| `multiblock_valid` | Мультиблок типа block_id собран | SpatialIndex::FindPattern / SimulationCore |
| `recipe_complete` | Машина завершила рецепт | MachineSystem / RecipeManager |

**Важно:** req_id=0 означает "любой" (например wrench_cycle на любую грань, pipe_flow по любой трубе).

### `quest_rewards.csv` — награды

```csv
quest_id,reward_type,reward_id,reward_count
q002,item,58,1
q003,item,278,1
q010,item,95,1
q011,item,90,1
q013,item,91,1
q060,unlock_era,2,
```

**reward_type:**
| Тип | Описание |
|-----|----------|
| `item` | Выдать предмет player_id в инвентарь |
| `unlock_era` | Явно разблокировать эпоху |
| `unlock_section` | Явно разблокировать секцию |

### `eras.json` — структура эпох и секций

```json
{
  "eras": [
    {
      "id": 0,
      "name": "Vagrant",
      "color": "#8B7355",
      "description": "First steps. Learn the basics of mining, crafting, and building.",
      "sections": [
        { "id": "foundation", "name": "Foundation" }
      ],
      "quest_ids": ["q001", "q002", "q003"]
    },
    {
      "id": 1,
      "name": "Apprentice",
      "color": "#5899DA",
      "description": "Electricity, machines, and automation fundamentals.",
      "sections": [
        { "id": "electric_tools", "name": "Electric Tools" },
        { "id": "machine_config", "name": "Machine Config" },
        { "id": "transport", "name": "Transport" }
      ],
      "quest_ids": ["q010", "q011", "q012", "q013", "q020", "q021", "q022", "q030", "q031"]
    },
    {
      "id": 2,
      "name": "Expert",
      "color": "#E67E22",
      "description": "Autonomous systems and full automation.",
      "sections": [
        { "id": "autonomous_mining", "name": "Autonomous Mining" },
        { "id": "processing", "name": "Ore Processing" }
      ],
      "quest_ids": ["q040", "q041", "q042", "q050"]
    },
    {
      "id": 3,
      "name": "Administrator",
      "color": "#9B59B6",
      "description": "Multiblocks and system mastery.",
      "sections": [
        { "id": "multiblocks", "name": "Multiblocks" }
      ],
      "quest_ids": ["q060", "q061"]
    }
  ]
}
```

---

## Architecture

### Где живёт

```
src/
├── data/
│   └── questbook/              ← CSV + JSON файлы (не генерируются, пишем руками)
│       ├── quests.csv
│       ├── quest_deps.csv
│       ├── quest_reqs.csv
│       ├── quest_rewards.csv
│       └── eras.json
│
├── services/
│   ├── simulation_core/
│   │   ├── QuestBook/
│   │   │   ├── QuestBookSystem.h      ← Загрузка, проверка, публикация
│   │   │   ├── QuestBookSystem.cpp
│   │   │   ├── QuestData.h            ← Структуры (Quest, QuestRequirement, etc.)
│   │   │   └── QuestData.cpp
│   │   └── main.cpp                   ← Регистрация QuestBookSystem
│   │
│   ├── game_client/
│   │   └── UI/
│   │       └── Windows/
│   │           └── player/
│   │               └── QuestBookWindow.h   ← ImGui окно
│   │               └── QuestBookWindow.cpp
│   │
│   ├── gateway/
│   │   └── Integration/
│   │       └── QuestBookIntegration.h   ← Маршрутизация запросов/ответов
│   │       └── QuestBookIntegration.cpp
│   │
│   ├── meta_db/                         ← SQLite: прогресс игрока
│   │   └── ...
│   │
│   └── message_router/                  ← Топики questbook.*
│       └── ...
```

### Data Flow

```
ИНИЦИАЛИЗАЦИЯ (при старте SimulationCore):
1. QuestBookSystem::load()
   → читает data/questbook/*.csv
   → читает data/questbook/eras.json
   → строит граф зависимостей в памяти
   → регистрирует обработчики событий для проверок

ЗАПРОС СТАТУСА (клиент открывает квестбук):
1. Client → Gateway: GatewayMessage(QUESTBOOK_REQUEST)
2. Gateway → SimulationCore: questbook.get.status
3. SimulationCore → MetaDB: SELECT * FROM quest_progress WHERE player_id = ?
4. MetaDB → SimulationCore: progress rows
5. SimulationCore → Gateway: QuestBookStatus(all_quests + progress)
6. Gateway → Client: QuestBookWindow рендерит дерево

ПРОВЕРКА КВЕСТА (автоматическая):
1. Любой ActionHandler (Craft, PlaceBlock, WrenchCycle...) завершает действие
2. Handler публикует событие: questbook.check.{quest_id}
3. QuestBookSystem::onQuestCheck() проверяет все reqs квеста
4. Если все reqs выполнены → отмечает complete
5. Публикует questbook.completed.{quest_id}
6. Проверяет dependences — разблокирует следующие квесты

РУЧНОЕ ЗАВЕРШЕНИЕ (manual_complete=1):
1. Client → Gateway: QuestBookCompleteAction(quest_id)
2. Gateway → SimulationCore: questbook.complete.request
3. QuestBookSystem::tryComplete() проверяет все reqs
4. Если ок → complete + разблокировка
5. Если нет → ответ с описанием что ещё нужно

НАГРАДЫ:
1. При complete → QuestBookSystem проверяет quest_rewards.csv
2. Если reward_type = item → giveItem(player_id, item_id, count)
3. Если reward_type = unlock_era → открывает эпоху
```

### Протокол (FlatBuffers)

Новые сообщения в `core.fbs` или отдельный `questbook.fbs`:

```fbs
namespace Protocol;

table QuestBookStatus {
  quests: [QuestStatus];
}

table QuestStatus {
  quest_id: string;
  status: QuestState;
  requirements: [QuestRequirementState];
}

enum QuestState: byte {
  LOCKED = 0;
  UNLOCKED = 1;
  IN_PROGRESS = 2;
  COMPLETED = 3;
}

table QuestRequirementState {
  req_type: string;
  req_id: uint;
  current_count: uint;
  required_count: uint;
  met: bool;
}

table QuestBookCompleteRequest {
  quest_id: string;
}

table QuestBookCompleteResponse {
  quest_id: string;
  success: bool;
  message: string;
  rewards: [ItemStack];
}
```

### Gateway топики

| Топик | Направление | Описание |
|-------|-------------|----------|
| `questbook.get.status` | Client → SimCore | Запрос статуса всех квестов |
| `questbook.status` | SimCore → Client | Статус всех квестов |
| `questbook.complete.request` | Client → SimCore | Запрос на завершение квеста |
| `questbook.complete.response` | SimCore → Client | Результат завершения |
| `questbook.check.{quest_id}` | Internal | Событие: проверить квест |
| `questbook.completed.{quest_id}` | Internal → Client | Квест завершён |

### Storage (MetaDB)

```sql
CREATE TABLE quest_progress (
    player_id TEXT NOT NULL,
    quest_id TEXT NOT NULL,
    status INTEGER NOT NULL DEFAULT 0,  -- 0=locked, 1=unlocked, 2=in_progress, 3=completed
    completed_at TEXT,
    PRIMARY KEY (player_id, quest_id)
);

CREATE TABLE quest_req_progress (
    player_id TEXT NOT NULL,
    quest_id TEXT NOT NULL,
    req_type TEXT NOT NULL,
    req_id INTEGER NOT NULL,
    current_count INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (player_id, quest_id, req_type, req_id)
);
```

---

## UI (ImGui)

```
┌────────────────────────────────────────────────┐
│  Quest Book                    [Era ▼] [Close] │
├────────────────────────────────────────────────┤
│                                                │
│  ── Vagrant ──                                 │
│                                                │
│  ✅ Welcome to GTNH                            │
│  ✅ Crafting Table                             │
│  ✅ First Pickaxe                              │
│                                                │
│  ── Apprentice ──                              │
│                                                │
│  🔓 Electric Tools                             │
│  │  ✅ Craft Wrench                            │
│  │  ✅ Craft ULV Drill                         │
│  │  🔓 Charge Your Drill  [Complete►]          │
│  │  🔒 Craft LV Drill                          │
│  │                                             │
│  🔓 Machine Config                             │
│  │  🔓 Place a Machine    [Complete►]          │
│  │  🔒 Configure Machine Side                  │
│  │  🔒 Connect Energy                          │
│  │                                             │
│  🔒 Transport                                  │
│                                                │
│  ── Expert (LOCKED) ──                         │
│                                                │
│  🔒 Autonomous Mining                          │
│  🔒 Ore Processing                             │
│                                                │
│  ── Administrator (LOCKED) ──                  │
│                                                │
│  🔒 Multiblocks                                │
│                                                │
└────────────────────────────────────────────────┘
```

**Элементы UI:**
- **Секции** — группировка квестов (Foundation, Electric Tools...)
- **Эпохи** — Vagrant, Apprentice, Expert, Administrator
- **Статусы квестов:**
  - `🔒 LOCKED` — серый, не нажимается
  - `🔓 UNLOCKED` — белый, можно открыть детали
  - `🔄 IN_PROGRESS` — жёлтый, часть reqs выполнена
  - `✅ COMPLETED` — зелёный, выполнено
- **Кнопка [Complete►]** — для ручного завершения (manual_complete=1)
- **Детали квеста** — при клике открывается панель с требованиями и прогрессом

---

## Integration Points

### Где добавляются проверки

| Система | Файл | Что добавляем |
|---------|------|---------------|
| CraftRequestHandler | `simulation_core/Crafting/CraftRequestHandler.cpp` | После успешного крафта: `publishQuestCheck("craft", item_id)` |
| SetBlockAction (CAS) | `simulation_core/ActionDispatcher.cpp` | После CAS OK: `publishQuestCheck("place_block", block_id)` |
| WrenchHandler | `simulation_core/Tools/WrenchHandler.cpp` | После cycleFace: `publishQuestCheck("wrench_cycle")` |
| PipeNetwork | `pipe_network/PipeNetworkService.cpp` | При item flow: `publishQuestCheck("pipe_flow")` |
| MachineSystem | `simulation_core/ECS/Systems/MachineSystem.cpp` | При завершении рецепта: `publishQuestCheck("recipe_complete")` |
| BatteryBufferSystem | `simulation_core/ECS/Systems/BatteryBufferSystem.cpp` | При зарядке: `publishQuestCheck("charge_tool", item_id, energy)` |
| EnergyDistribution | `pipe_network/PipeNetwork.cpp` | При передачи энергии: `publishQuestCheck("energy_flow", amount)` |

### MessageRouter топики

```cpp
// SimulationCore main.cpp — регистрация QuestBookSystem
auto qbs = std::make_unique<QuestBookSystem>(registry, *mr_client);
mr_client->subscribe("craft.completed", [qbs](auto& msg) { qbs->onCraftCompleted(msg); });
mr_client->subscribe("block.placed", [qbs](auto& msg) { qbs->onBlockPlaced(msg); });
mr_client->subscribe("tool.action", [qbs](auto& msg) { qbs->onToolAction(msg); });
mr_client->subscribe("energy.flow", [qbs](auto& msg) { qbs->onEnergyFlow(msg); });
mr_client->subscribe("pipe.flow", [qbs](auto& msg) { qbs->onPipeFlow(msg); });
mr_client->subscribe("recipe.completed", [qbs](auto& msg) { qbs->onRecipeCompleted(msg); });
```

---

## Implementation Plan

### Phase 1 — Data Layer (1 день)

1. Создать `data/questbook/` с CSV + JSON файлами (шаблоны выше)
2. Создать `QuestData.h/.cpp` — структуры для загрузки/хранения
3. `QuestBookSystem::load()` — парсинг CSV + JSON, построение графа

### Phase 2 — Storage + Core Logic (1 день)

4. `QuestBookSystem::getStatus(player_id)` — запрос прогресса из MetaDB
5. `QuestBookSystem::checkQuest(player_id, quest_id)` — проверка всех reqs
6. `QuestBookSystem::tryComplete(player_id, quest_id)` — завершение + награды
7. `QuestBookSystem::unlockDependents(player_id, quest_id)` — разблокировка

### Phase 3 — Integration (1 день)

8. Подписка на события (craft, place, wrench, pipe, energy, recipe)
9. Gateway топики `questbook.*`
10. `QuestBookIntegration` в Gateway

### Phase 4 — UI (1 день)

11. `QuestBookWindow.h/.cpp` — ImGui окно с деревом
12. Соединение с NetClient (запрос статуса, отправка complete)

**Итого: ~4 дня на MVP**

---

## Отложено на потом

| Фича | Причина |
|------|---------|
| OR-зависимости (выполнить A или B) | Пока не нужно, усложняет граф |
| Разветвлённые деревья (выбор пути) | Пока линейная прогрессия |
| Кастомные иконки для квестов | Используем item_id как иконку |
| Анимации в UI | Пока статичное дерево |
| Поиск по квестам | Пока мало квестов |
| Quest chains (A→B→C авто-старт) | Пока ручное завершение |
