# Heat Generator Bug — Full Analysis & Fixes

## Симптомы

1. При помещении coal в heat_generator — энергия не производится (показывает 0)
2. Машина показывает "EU" вместо "HU" (неправильный тип энергии)
3. Heat furnace рядом: "connection to machine lost — state may be stale"
4. После рестарта сервера — машины снова показывают EU (до перестановки)
5. Даже после перестановки — иногда EU

## Найденные баги (все починены)

### 1. MachineSlotHandler — хардкод `EnergyType::ELECTRICITY`

**Файл:** `src/services/simulation_core/Actions/MachineSlotHandler.cpp:102`

```cpp
// БЫЛО:
events_->publishBlockEntityUpdate(..., EnergyType::ELECTRICITY, ...);
// СТАЛО:
EnergyType etype = EnergyType::ELECTRICITY;
if (auto* es = reg.try_get<EnergyStorage>(entity)) {
    etype = es->type;
}
events_->publishBlockEntityUpdate(..., etype, ...);
```

Ответ клиенту при слоте всегда шёл с `ELECTRICITY`. Теперь читает реальный тип из EnergyStorage компонента.

### 2. onMachineCreated — не читал energy type

**Файл:** `src/services/simulation_core/main.cpp:247`

```cpp
// БЫЛО:
eventPublisher->publishBlockEntityUpdate(x, y, z, machine_id, {}, 0.0f, 0);
//                                                    ^ energy_type не указан → ELECTRICITY

// СТАЛО:
auto machEntity = findEntityAt(simulationEngine->reg(), x, y, z);
if (machEntity != entt::null) {
    if (auto* es = simulationEngine->reg().try_get<simcore::EnergyStorage>(machEntity)) {
        etype = es->type;
    }
} else if (auto* reg = MachineRegistry::instance()) {
    if (auto* info = reg->Get(machine_id)) {
        if (info->energy_in.has_value()) etype = info->energy_in.value();
        else if (info->energy_out.has_value()) etype = info->energy_out.value();
    }
}
eventPublisher->publishBlockEntityUpdate(..., etype);
```

При создании машины — шлёт правильный тип из EnergyStorage (который уже создан с типом из MachineRegistry).

### 3. GeneratorSystem — fallback если `maxOutput=0`

**Файл:** `src/services/simulation_core/ECS/Systems/GeneratorSystem.cpp:63`

```cpp
// БЫЛО:
int32_t produced = std::min(energy.maxOutput, remaining);
// СТАЛО:
int32_t rate = energy.maxOutput > 0 ? energy.maxOutput : 32;
int32_t produced = std::min(rate, remaining);
```

MachineRegistry для heat_generator возвращает `maxOutput=0` (хотя в YAML `max_output: 32`). Причина не выяснена — возможно битый YAML на диске или баг парсинга. Fallback гарантирует production rate=32.

### 4. HeatTransferSystem — проверка `energy_out` в дополнение к `role`

**Файл:** `src/services/simulation_core/ECS/Systems/HeatTransferSystem.cpp:44`

```cpp
// БЫЛО:
if (!info || info->role != MachineRole::PRODUCER) continue;
// СТАЛО:
if (!info) continue;
bool isProducer = info->role == MachineRole::PRODUCER
               || (info->energy_out.has_value()
                && info->energy_out.value() == EnergyType::HEAT);
if (!isProducer) continue;
```

Из-за битого MachineRegistry (role=CONSUMER вместо PRODUCER для heat_generator), HeatTransferSystem не видел генератор как источник тепла. Дополнительная проверка `energy_out: HEAT` это обходит.

### 5. onMachineCreated — вызывался ДО создания EnergyStorage

**Файл:** `src/services/simulation_core/ECS/SimulationEngine.cpp`

```cpp
// БЫЛО (порядок):
// 1. MachineComponent создан
// 2. → onMachineCreated fires (EnergyStorage ещё НЕТ → etype=ELECTRICITY)
// 3. RecipeProgress, InventoryContainer, EnergyStorage созданы
//
// СТАЛО (порядок):
// 1. MachineComponent создан
// 2. RecipeProgress, InventoryContainer созданы
// 3. EnergyStorage создан (с правильным etype из MachineRegistry)
// 4. → onMachineCreated fires (EnergyStorage УЖЕ ЕСТЬ → читает правильный тип)
```

`onMachineCreated` перенесён ПОСЛЕ создания всех компонентов.

### 6. MachineSystem — forcePublish на первых тиках

**Файл:** `src/services/simulation_core/ECS/Systems/MachineSystem.h + .cpp`

```cpp
// MachineSystem.h:
int startupTicks_ = 3;  // новый счётчик

// MachineSystem.cpp:
// Первые 3 тика — forcePublish для всех машин
// Каждые 10 тиков после — forcePublish (было 100)
// При изменении инвентаря — publish (было всегда)
```

Это нужно чтобы клиент, подключившийся после старта сервера, быстрее получил BlockEntityUpdate с правильным типом. Первый forcePublish при старте клиент пропускает (ещё не подключён), второй и третий ловит.

### 7. Lazy entity creation — MachineSlotHandler

**Файл:** `src/services/simulation_core/Actions/MachineSlotHandler.cpp`

Если ECS-entity не существует при слоте (после рестарта), MachineSlotHandler запрашивает блок из ChunkStore и создаёт entity через `engine_->onBlockChanged`. Первый клик вернёт ошибку "No machine", но entity будет создан. Следующий клик работает.

### 8. SetBlockCASHandler — не отправлял BlockEntityUpdate при правом клике

**Файл:** `src/services/simulation_core/Actions/SetBlockCASHandler.cpp:91-101`

```cpp
// ДОБАВЛЕНО:
publisher_->publishBlockEntityUpdate(x, y, z, expected_block_id, {}, 0.0f, 0, clickEtype);
```

При правом клике на машину — форсирует отправку BlockEntityUpdate с правильным типом энергии. Но этот код НЕ ВЫПОЛНЯЕТСЯ при текущей логике клиента (см. Баг #9).

### 10. ParseRole — регистро-зависимый (КОРЕНЬ бага maxOut=0 / role=CONSUMER)

**Файл:** `src/libs/machine_registry/MachineRegistry.cpp:17`

```cpp
// БЫЛО: сравнивает только с UPPERCASE
inline MachineRole ParseRole(const std::string& str) {
    if (str == "CONSUMER")    return MachineRole::CONSUMER;
    if (str == "PRODUCER")    return MachineRole::PRODUCER;
    return MachineRole::CONSUMER;   // ← всё не-UPPERCASE → CONSUMER
}

// СТАЛО: case-insensitive (toupper перед сравнением)
```

В `data/registry/machines.yaml` ВСЕ `role:` значения в нижнем регистре (`producer`/`consumer`).
`ParseRole("producer")` не матчился → **каждый производитель** (heat_generator,
creative_generator, rotare_generator, оба boiler'а) загружался как `CONSUMER` →
`maxOutput=0`, `maxInput=usage-default=32`. Это и есть причина из хронологии
(`maxOut=0 maxIn=32` = ветка CONSUMER парсера, при том что в YAML `role: producer`).

Побочный эффект: 13 потребителей работали «по удаче» — `"consumer"` (lowercase)
тоже не матчился и падал в дефолт CONSUMER. Теперь оба разбора корректны.

Исправление ParseRole делает фиксы #3 (fallback 32) и #4 (проверка `energy_out`)
неактуальными по корню — они остаются как дефенсивные заглушки.

### 11. Пре-существующие машины не получали ECS entity (КОРЕНЬ «тепло не доходит» в живой игре)

**Файл:** `src/services/simulation_core/Actions/SetBlockCASHandler.cpp`

**Симптом:** unit-тест передачи тепла проходит, но в живой игре печь рядом с
горящим генератором остаётся холодной. Генератор при этом заполнялся до
`10000/10000` и переставал жечь уголь (`energy.isFull()`).

**Корень:** SimCore создаёт entity-машины ТОЛЬКО при событии изменения блока
(`onBlockChanged`). Блоки, которые существовали в мире ДО старта данного
инстанса simcored (расставлены в прошлой сессии, мир персистится в chunkdb),
никогда не получают entity. Для HeatTransferSystem/GeneratorSystem/MachineSystem
машина без entity невидима → тепло не передаётся.

Почему unit-тест проходил: он создавал entity через `onBlockChanged` напрямую
(блоки «поставлены» в тесте). В реальной игре печи были расставлены до рестарта.

**Доказательство из лога:** за всю сессию `onBlockChanged` вызван один раз
(генератор, поставлен в этой сессии). По 7 правым кликам по печам (57344) —
ни одного создания entity.

**Фикс:** в `SetBlockCASHandler` правый клик по машине без entity лениво
создаёт её из ChunkStore (паттерн как в MachineSlotHandler) и публикует
реальную энергию из EnergyStorage (а не 0). Регрессионный тест
`test_SetBlockCASHandler_lazy_creates_pre_existing_machine`.

**Проверено в живой игре (12:32):** правый клик по печам создал entity,
`[HeatTransfer] 57344 → 1 transferred 24 heat`, печи набрали `energy=9996 type=1`,
генератор `coal=30722 energy=0/10000` (отдаёт, не копит).

### 9. Клиент не слал right-click на машину (ПОЧИНЕНО)

**Файл:** `src/services/game_client/GameClient.cpp` + `UI/UIDefaults.h/.cpp`

**Реальная проблема была хуже, чем «неправильная позиция»:** правый клик по машине
открывал MachineWindow локально через `TryOpenBlockUI`, после чего
`uiMgr_.AnyOpen()==true` → `InteractionSystem::Update()` вообще пропускался →
`SendBlockAction(RIGHT_MOUSE_CLICK)` НЕ отправлялся. Сервер никогда не узнавал о
правом клике → хендлер из бага #8 (`publishBlockEntityUpdate` при right-click)
никогда не срабатывал → окно висело на устаревшем состоянии (EU, пустые слоты,
«connection to machine lost»).

**Фикс:**
- `TryOpenBlockUI()` теперь возвращает `IUIWindow*` (открытое окно или nullptr)
  вместо `bool`.
- В `GameClient::Update()` после открытия окна: если `dynamic_cast<MachineWindow*>(opened)`
  — отправить `SendBlockAction(RIGHT_MOUSE_CLICK, pos, blockId, new_block_id=0)`.
  Сервер отвечает свежим `BlockEntityUpdate` с правильным типом энергии.
- Crafting table (тоже в machines.yaml) исключён автоматически — его окно
  `CraftingWindow`, а не `MachineWindow`, поэтому dynamic_cast не матчится.

## Хронология расследования

1. Пользователь: "уголь схавался но везде 0"
2. Лог: `[GeneratorSystem] energy fields: maxOut=0 maxIn=32 cap=10000 cur=0 type=1`
   - maxOut=0 при YAML `max_output: 32` — MachineRegistry отдаёт битые данные
   - capacity=10000 — ок
   - type=1 (HEAT) — ок
3. Добавлен fallback production rate (32 если maxOutput=0) — генератор начал заряжаться
4. HeatTransferSystem не видел генератор как PRODUCER — добавлена проверка `energy_out`
5. onMachineCreated читал EntityType не из EnergyStorage — починено
6. EnergyStorage создавался ПОСЛЕ onMachineCreated — порядок исправлен
7. BlockEntityUpdate отправлялся ДО подключения клиента — добавлен forcePublish на первых тиках
8. Правый клик на машину вообще не отправлялся (окно открывалось локально, `InteractionSystem` пропускался) — ПОЧИНЕНО, см. баг #9
9. MachineSlotHandler лениво создаёт entity при первом слоте после рестарта
10. **КОРЕНЬ maxOut=0/role=CONSUMER найден:** `ParseRole` регистро-зависим, а в YAML всё в нижнем регистре — см. баг #10

## Где копать дальше

1. ~~**Почему MachineRegistry возвращает maxOut=0 и role=CONSUMER?**~~ → **РЕШЕНО**: `ParseRole` регистро-зависим (`MachineRegistry.cpp:17`), YAML использует lowercase `producer`/`consumer`. Исправлено case-insensitive разбором. Касается всех производителей (heat/creative/rotare generator, boiler'ы).

2. ~~**Клиент шлёт неправильную позицию**~~ → **РЕШЕНО**: проблема была глубже — правый клик на машину не отправлялся вовсе (окно открывалось локально). Теперь `GameClient::Update` шлёт `RIGHT_MOUSE_CLICK` на позицию машины при открытии `MachineWindow`.

3. **MessageRouter теряет BlockEntityUpdates** — после старта сервера клиент подключается и не получает старые апдейты. Теперь при каждом открытии `MachineWindow` клиент шлёт right-click → сервер публикует свежий `BlockEntityUpdate` (гарантированный механизм, не полагается на тайминг force-publish). Резервные варианты, если этого окажется мало:
   - Gateway кэширует последний BlockEntityUpdate по позиции и отдаёт при открытии окна
   - Force-publish на MachineSystem каждые 10 тиков (реализовано, но не гарантирует)

## Файлы изменений

```
src/services/simulation_core/Actions/MachineSlotHandler.cpp
src/services/simulation_core/Actions/SetBlockCASHandler.cpp
src/services/simulation_core/ECS/SimulationEngine.cpp
src/services/simulation_core/ECS/Systems/GeneratorSystem.cpp
src/services/simulation_core/ECS/Systems/HeatTransferSystem.cpp
src/services/simulation_core/ECS/Systems/MachineSystem.h
src/services/simulation_core/ECS/Systems/MachineSystem.cpp
src/services/simulation_core/Network/SimCoreMessageHandler.cpp
src/services/simulation_core/main.cpp
```

### Дополнительно (этот проход)

```
src/libs/machine_registry/MachineRegistry.cpp          # ParseRole case-insensitive (корень бага)
src/services/game_client/GameClient.cpp                # right-click → RIGHT_MOUSE_CLICK при открытии MachineWindow
src/services/game_client/UI/UIDefaults.h               # TryOpenBlockUI → IUIWindow*
src/services/game_client/UI/UIDefaults.cpp
src/services/simulation_core/Actions/SetBlockCASHandler.cpp  # lazy-create entity для пре-существующих машин (баг #11)
src/services/simulation_core/test/test_ecs_systems.cpp # регрессия lowercase role + end-to-end heat + lazy-create
```

### Распространение тепла — подтверждено (2026-08-07)

End-to-end проверка через реальный путь `onBlockChanged` + реальный `machines.yaml`:
`heat_generator` (1110:00:2, PRODUCER/HEAT, maxOutput=32) горит на угле → HeatTransferSystem
передаёт тепло в соседнюю `heat_furnace` (1110:00:0, CONSUMER/HEAT) — печь накапливает
~28/tick (32 передано − 4 остывание). Регрессионный тест
`test_HeatTransferSystem_yaml_generator_to_furnace`. Тепло видно в шкале печи.
**Решение (пользователь): рецепты печи не трогать** — `furnace.yaml` `eu≈0` (energy_cost≈0),
т.е. печь не потребляет тепло из буфера, но и без него тепло отображается.

**LIVE-подтверждение (12:32, боевой сервер после рестарта в 12:30):**
- правый клик по печам → ленивое создание entity (см. баг #11);
- `[HeatTransfer] 57344 → N transferred 24/4/4 heat` каждые 50ms;
- печи набрали `energy=9996 type=1 (HEAT)`, `energy=1476 type=1`;
- генератор `coal=30722 energy=0/10000` — жжёт уголь и отдаёт всё тепло
  (до фикса копил до 10000/10000 и останавливался).

Для воспроизведения после рестарта: правый клик по генератору и каждой печи
создаёт entity (первый клик), положить уголь в генератор — тепло потечёт.
```
