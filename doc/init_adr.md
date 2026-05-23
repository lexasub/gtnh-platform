# ADR: Initial Architecture Decisions

**Дата**: Июнь 2026
**Статус**: Принят (некоторые решения пересмотрены — см. ниже)

> ⚠️ **Исторический документ.** Ряд решений был изменён в процессе реализации.
> Подробные risk analysis перемещены в `doc/archive/`.

---

## 1. MessageRouter: доставка и подписки

### 1.1 Self-echo запрещён (также нужно проверить что роутер не шлет инфу тем кто на топик не подписался)

**Контекст**: При pub/sub-модели сервис, публикующий сообщение в топик, на который он же подписан, по умолчанию получит своё же сообщение.

**Решение**: Router НЕ должен доставлять сообщение отправителю. Если сервис A опубликовал сообщение в топик T, и сервис A подписан на T — Router игнорирует эту доставку.

**Обоснование**: SimulationCore не должен получать свои же `PlayerAction` или `BlockChanged`. Это устраняет бесконечные циклы и лишнюю обработку.

### 1.2 Typed message bus (сейчас), не строки

**Решение**: Сообщения идентифицируются `uint8_t type_id` в заголовке, Router коммутирует по `type_id`, не по строковым топикам.

**Почему не FlatBuffers union**: Пока <20 типов сообщений, простой switch быстрее и не требует перекомпиляции схемы при каждом добавлении. Миграция на union возможна позже — ценою рефакторинга всех хендлеров.

### 1.3 Доставка: at-least-once с ack

**Решение**: Для state-changing сообщений (`BlockChanged`, `MultiblockCreated`) получатель шлёт `Ack`. Router повторяет доставку при отсутствии Ack. Для стриминговых (`ChunkSnapshot`, `EntitySnapshot`) — at-most-once.

### 1.4 WAL — нет (MVP)

**Решение**: Без WAL. При рестарте Router теряет недоставленные сообщения. Сервисы восстанавливают состояние при старте самостоятельно.

---

## 2. EntityStateStore

### 2.1 Отдельный сервис

**Решение**: EntityStateStore — отдельный C++ сервис (позже возможно Go). Не часть ChunkStore.

**Обоснование**: ChunkStore — dumb block dump. TileEntity-состояния машин, верстаков, сундуков требуют сервиса с частыми записями по ключу `dim|x|y|z`. Смешивание нарушает принцип единственной ответственности.

### 2.2 LMDB (реализовано, in-memory пропущен)

**Решение**: Вопреки ADR, реализация сразу использует LMDB (mdb_dbi, mdb_put/mdb_get), не проходя через in-memory этап. Оправдано — LMDB zero-copy чтение и ACID без лишнего кода.

**Ключ**: `pack(dim, x, y, z) → uint64_t`

### 2.3 RPC

```
GetState(key: uint64) → Blob
SetState(key: uint64, blob: Blob)
Release(key: uint64)        // при unload чанка
```

---

## 3. RecipeManager

### 3.1 Встроен в SimulationCore (пересмотрено)

**Решение**: RecipeManager — часть SimulationCore (embedded), не отдельный сервис. recipe.fbs определяет RPC (CheckRecipeReq, CraftReq), но сервер не развёрнут. Рецепты грузятся из JSON напрямую.

**Обоснование**: Разделение ответственности. RecipeManager может рестартовать независимо. В будущем — hot-reload рецептов без остановки SimulationCore.

### 3.2 Взаимодействие через Router

```
SimulationCore → Router → RecipeManager.CheckRecipe
RecipeManager  → Router → SimulationCore (response)
```

Router выступает point-to-point transport'ом (request-response через correlationId).

### 3.3 Кеширование рецептов (L1) в SimulationCore

**Решение**: SimulationCore кеширует локально результаты `CheckRecipe` для каждой машины. Кеш инвалидируется при изменении входного инвентаря машины. Это убирает RPC на каждый тик для машин с неизменным инвентарём.

---

## 4. ChunkStore — dumb block storage

### 4.1 ChunkStore НЕ знает о машинах

**Решение**: ChunkStore хранит только `block_id + meta + mb_id`. Он не интерпретирует `mb_id` — это просто число. ChunkStore не отвечает за:
- Тип машины (furnace, macerator)
- Состояние обработки
- Инвентарь машины
- Валидность мультиблока

### 4.2 Координация unload

```
ChunkStore → SimCore: ChunkUnloadRequest(dim, cx, cy, cz)
SimCore  → ChunkStore: ChunkUnloadResponse(released_mb_ids[], hold_mb_ids[])
```

ChunkStore удаляет чанк только когда ВСЕ `mb_id` получили `release`. Если SimCore вернул `hold` (якорь мультиблока вне чанка) — чанк остаётся в памяти.

---

## 5. SimulationCore: Machine Tick и стриминг состояния

### 5.1 Tick — polling + needsTick

**Решение**: SimulationCore итерирует все машины каждый тик (20 Hz), но idle-машины (`needsTick = false`) пропускают recipe lookup. `needsTick` взводится при:
- Изменении входного инвентаря (событие от ChunkStore)
- Изменении энергии (событие от PipeNetwork)

### 5.2 Стриминг состояния клиенту — по подписке

**Проблема**: Когда игрок стоит рядом с машиной, сервер не должен слать per-tick обновления состояния, если игрок не открыл GUI.

**Решение**: Два уровня обновлений:

| Уровень | Триггер | Частота | Что шлём |
|---------|---------|---------|----------|
| Базовый | Игрок в радиусе interest | 1–2 Hz | `isRunning` (флаг бежит/стоит) |
| Детальный | GUI машины открыт | Каждый тик (20 Hz) | прогресс, энергия, слоты |

**Реализация**:
- Клиент шлёт `WatchEntity(x,y,z)` / `UnwatchEntity(x,y,z)` при открытии/закрытии GUI
- SimulationCore добавляет/убирает машину из списка "горячего стриминга" для этого игрока
- Список `watched` хранится в ECS как компонент `PlayerWatchedMachines{watched: [coord]}`

### 5.3 Мультиблоки — в ECS (пересмотрено)

**Решение**: Мультиблоки хранятся в ECS как `MultiblockController{mb_id, anchor, blocks}` компоненты в `entt::registry`. Отдельный `MultiblockRegistry` не понадобился — EnTT sparse set и так O(1) lookup.

---

## 6. Gateway

### 6.1 io_uring

**Решение**: Gateway использует Asio с io_uring на Linux. В разработке.

### 6.2 Нет кеша чанков

**Решение**: Gateway НЕ кеширует чанки. Это не "лёгкая логика". Gateway — TCP mux/demux с минимальным уровнем game-логики.

### 6.3 Маршрутизация

```
PlayerAction  → SimulationCore (через Router)
InventorySync → MetaDB (через Router)
ChunkRequest  → ChunkStore (через Router)
```

---

## 7. Протокол

### 7.1 type_id dispatch (остаётся)

**Решение**: Используем `uint8_t type_id` + switch. Пока <20 типов — переделывать на FlatBuffers union преждевременно.

**Риск при миграции**: Потребуется рефакторинг всех хендлеров. Принимаем. Когда типов станет >20 — проведём миграцию за один коммит.

### 7.2 Version field

**Решение**: Добавить `uint16_t version` в заголовок каждого сообщения. Позволяет эволюционировать протокол без поломки старых клиентов.

### 7.3 LZ4 — нет (MVP)

**Решение**: Без сжатия на MVP. Замерить профиль трафика сначала. Если Gateway→Client узкое место — добавить LZ4 там.

---

## 8. ECS Design (SimulationCore)

### 8.1 Granular components

```
PositionComponent     { world_x, world_y, world_z }   // immutable
InventoryComponent    { slots[capacity] }              // mutable
MachineStateComponent { isRunning, progress, recipeId } // mutable  
EnergyStorageComponent { eu, maxEu, tier }             // mutable
```

### 8.2 Coordinate → Entity lookup

**Решение**: `std::unordered_map<uint64_t, entt::entity>` — ключ `pack(x,y,z)`, значение entity. Обновляется при создании/удалении TileEntity. O(1) lookup.

---

## 9. Технический долг / отложено

| Решение | Когда | Причина |
|---------|-------|---------|
| FlatBuffers union для сообщений | >20 типов | Рефакторинг всех хендлеров |
| ~~LMDB для EntityStateStore~~ | ✅ Уже сделано | ADR предполагал in-memory → LMDB; реализация пошла сразу в LMDB |
| WAL для Router | Потеря сообщений стала проблемой | Лишняя сложность сейчас |
| LZ4 на чанках | Gateway→Client узкое место | Замерить сначала |
| Hot-reload рецептов | RecipeManager отдельный сервис | Нужен механизм перезагрузки JSON без рестарта |
| Multi-dimension | Когда >2 измерений | Overhead изоляции не стоит выгоды для 1-2 dim |
| Redstone / automation / AE2 | Когда игра играбельна | Layer 3 |

---

*Первичная запись архитектурных решений (июнь 2026). Изменения отмечены в тексте.*

**Изменённые решения (пересмотр в процессе реализации):**
- EntityStateStore: in-memory для MVP → сразу LMDB
- RecipeManager: отдельный сервис → встроен в SimulationCore
- Мультиблоки: отдельный MultiblockRegistry → в ECS (entt::registry)

Подробные risk-analysis по этим решениям — в `doc/archive/`.
