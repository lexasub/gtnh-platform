# Change: Init Game Flow — начальная подготовка игрока в survival-режиме

## Why

Игрок появляется без стартового набора и направляющей: инвентарь пуст, квестбук не открыт,
режим — CREATIVE (летает, сломать/поставить блоки можно). Нет механики «начать игру».
`/startGameScenario 0` должен: очистить инвентарь, выдать стартовый набор, переключить в
SURVIVAL и открыть квестбук на эпохе Vagrant.

В перспективе — система сценариев (не только начальный, но и «начать с паровой эпохи» и т.д.),
поэтому точка входа делается расширяемой.

**Ключевое решение:** сценарий исполняется **на сервере** (SimulationCore), клиент — тонкий
запросчик. Причина — inventory sync нельзя надёжно сделать client-authoritative: в протоколе нет
атомарной операции «очистить/заменить», а сервер пушит снапшоты, которые перезапишут локальные
изменения (см. design.md D1).

## Контекст

- `ConsoleWindow` — реестр команд через `RegisterCommand()` (`ConsoleWindow.cpp:68-74`);
  сигнатура `CommandFn(cw, playerInv, args)`. Прецедент строгой валидации — `/gamemode`
  (`ConsoleWindow.cpp:27-51`).
- `GameMode` enum (`Common/Inventory.h:16-21`); `InventoryState::gameMode` по умолчанию `CREATIVE`
  (`Common/Inventory.h:60`).
- Сервер **уже** обрабатывает game mode: `kGameModeChange`(30) → gateway → `player.gamemode.change`
  → `PlayerInventoryStore::setGameMode` + echo (`SimCoreMessageHandler.cpp:268-287`). Т.е. wire
  и хранение режима существуют; сценарий добавляет **исполнение**, а не транспорт.
- `PlayerInventoryStore` (`simulation_core/Storage/PlayerInventoryStore.h`): `setSlots` (полная
  замена → onChange в MetaDB + postMutation → полный снапшот на `player.inventory.update`),
  `giveItem` (стакинг), `setGameMode`.
- Квестбук открывается по Q; `selectedEra_` по умолчанию 0 = VAGRANT (`QuestBookWindow.h:36`).
- C4: `ConsoleWindow`/`QuestBookWindow` отсутствуют на `doc/c4/level4-client-ui-windows.puml`.

## What Changes

### 1. Protocol: StartScenarioReq / StartScenarioResp
- `core.fbs`: таблицы `StartScenarioReq` (player_id, scenario_index) и `StartScenarioResp`
  (player_id, scenario_index, success, error, game_mode, quest_book_era).
- `gateway.fbs` union + C++-константы: **kStartScenarioReq = 31, kStartScenarioResp = 32**
  (следующие свободные после `kGameModeChange = 30`). Индексы fbs-union устарели относительно
  C++-констант (документировано в `gateway.fbs:22-27`) — правит wire именно C++.

### 2. Server (SimulationCore) — исполнение сценария
- Таблица сценариев (данные, не код) в новом `simulation_core/Scenario/GameScenario.h/.cpp`:
  index, name, targetMode, giveItems, clearFirst, questBookEra.
- Обработчик `player.scenario.start`: валидация (pid ≠ 0, index в диапазоне) → исполнение:
  `setSlots(пустой)` → `giveItem(...)` × N → `setGameMode(targetMode)` → публикация
  `player.scenario.start.response`.
- Инвентарь уходит клиенту через **существующий** push: postMutation → `player.inventory.update`
  → `kInventoryUpdate`(6). Гейм-мод несёт сам resp. Публикация `player.inventory.update` происходит
  **до** resp — клиент гарантированно получает снапшот раньше ack.
- `setSlots`/`giveItem` автоматически персистят каждый слот в MetaDB через `onChange`.

### 3. Gateway — маршрутизация
- `kStartScenarioReq` → publish `player.scenario.start`.
- subscribe `player.scenario.start.response` → send `kStartScenarioResp` клиенту
  (тот же паттерн relay, что `player.gamemode.changed`, `gateway.cpp:420-421`).

### 4. Client (game_client)
- `NetClient`: `SendStartScenarioReq(pid, index)` + callback на resp.
- `ConsoleWindow`: `RegisterCommand("startGameScenario")` со строгой валидацией как у `/gamemode`
  (пустой аргумент / не-число / индекс вне диапазона → ошибка в консоль, без запроса).
- `GameScenario.h/.cpp` (новый): **display-only** таблица (name, questBookEra, outputMessage) —
  для `/help` и вывода в консоль. Исполнение — только на сервере.
- По успешному resp: применить `gameMode` из resp, программно открыть `QuestBookWindow` и выбрать
  эпоху из resp.

### 5. Scenario 0 (стартовый набор)
- Верстак: `0:10:11:1` → **packed 22529** (не flat-ид «14» — в `items.csv` flat-идов нет).
- Деревянная кирка: `0:11110:3` → **packed 30723**.
- Целевой режим SURVIVAL, эпоха VAGRANT, `clearFirst = true`.
- Набор данных в таблице — расширяемый; добавление кирки/топора = строка в таблице, не код.

## Impact

### Affected specs
- `protocol` — ADDED: StartScenarioReq/Resp
- `player-interaction` — ADDED: серверное исполнение сценария
- `questbook` — ADDED: открытие квестбука при старте сценария
- `questbook-era-transition` — без изменений (VAGRANT уже дефолт, сценарий лишь показывает её)
- `game-modes` (change `add-game-modes`, не заархивирован) — зависимость: resp несёт `GameMode`,
  клиент применяет его в `InventoryState::gameMode`. Примечание: echo-путь
  (`kGameModeChange` → `onGameModeChange_`) существует в `NetClient`, но колбэк пока никем не
  зарегистрирован — задача 5.3 станет первым живым применением wire-мода на клиенте; регистрация
  echo-колбэка — отдельный мини-фикс вне этого изменения

### Affected code

| Файл | Изменение |
|------|-----------|
| `src/protocol/core.fbs` | ADD: `StartScenarioReq` / `StartScenarioResp` tables |
| `src/protocol/gateway.fbs` | ADD: union members |
| `src/services/gateway/gateway.h` | ADD: `kStartScenarioReq = 31`, `kStartScenarioResp = 32` |
| `src/services/gateway/gateway.cpp` | route req → `player.scenario.start`; relay resp |
| `src/services/gateway/main.cpp` | subscribe `player.scenario.start.response` |
| `src/services/simulation_core/Scenario/GameScenario.h/.cpp` | **Новый** — таблица сценариев |
| `src/services/simulation_core/Network/SimCoreMessageHandler.cpp` | handler `player.scenario.start` |
| `src/services/simulation_core/main.cpp` | subscribe `player.scenario.start` |
| `src/services/game_client/Network/NetClient.h/.cpp` | `SendStartScenarioReq` + resp callback |
| `src/services/game_client/UI/Windows/player/ConsoleWindow.cpp` | `RegisterCommand("startGameScenario")` |
| `src/services/game_client/UI/Windows/player/GameScenario.h/.cpp` | **Новый** — display-таблица + обработка resp |
| `src/services/game_client/UI/Windows/player/QuestBookWindow.h/.cpp` | `SetEra(int)` / программное открытие |

### Affected data
- `data/quests/quests.csv`, `data/quests/quest_graph.json` — **не меняются**
- `data/registry/items.csv` — **не меняется** (используем существующие id)

### Affected C4 diagrams
- `doc/c4/level4-client-ui-windows.puml` — добавить ConsoleWindow, QuestBookWindow, GameScenario
  в «Окна игрока»

### Non-goals
- ❌ **Sub-eras (data-driven).** `Era → SubEra → Section → Quests`, новый файл данных, UI
  sub-era табов — отдельная фича
- ❌ Права на команду (permission check). Любой подключённый клиент может вызвать
  `/startGameScenario` — сценарий исполняется сервером, но авторизация команды отложена
- ❌ Сценарии «начать с паровой эпохи», «пропустить руду» — будут другими номерами позже
- ❌ Перенос/сохранение инвентаря между сценариями
- ❌ Создание новых предметов/блоков, правка quests.csv
