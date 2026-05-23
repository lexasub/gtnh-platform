# SimulationCore: Архитектурное решение

**Дата**: 2026-06-21
**Контекст**: Обсуждение C4-диаграмм, поиск узких мест
**Участники**: Sisyphus (AI), пользователь

---

## Проблема

SimulationCore — единственный сервис, через который проходит **вся логика мира**:
- ECS тик (20Hz) — машины, генераторы, котлы, теплопередача
- Крафт — проверка рецептов, сопоставление паттернов, мутация инвентаря
- Обработка действий — SetBlockCAS, PlayerAction, CraftRequest
- Сеть — полдюжины TCP клиентов (Router, ChunkStore, ESS, PipeNetwork, MetaDB)
- Публикация событий — BlockChanged, MultiblockCreated, ChunkSnapshot

В C4-диаграммах это видно: SimCore имеет 7+ внешних связей и ~30+ внутренних компонентов.
Ни один другой сервис не имеет такого скопления логики.

## Попытка split'а

Рассматривалось разделение SimCore на два сервиса:

```
SimCore-A (ECS + тики)
  └── MachineSystem, GeneratorSystem, BoilerSystem, HeatTransferSystem
SimCore-B (крафт + инвентарь)
  └── CraftRequestHandler, CraftInventoryFlow, InventoryActionHandler
```

### Почему это не работает

Проблема — **связность на уровне ECS-компонентов**. Они не принадлежат одному "слою", они перемешаны:

| Компонент | Используется где |
|-----------|-----------------|
| `InventoryContainer` | Машины (вход/выход), игрок (инвентарь), крафт (слоты) |
| `RecipeProgress` | MachineSystem (тик), CraftRequestHandler (запуск) |
| `MachineComponent` | MachineSystem, ActionDispatcher, HeatTransferSystem |
| `EnergyStorage` | MachineSystem (расход), GeneratorSystem (запас) |

Разделение на два сервиса означает, что каждый чих крафта требует RPC для чтения/записи `InventoryContainer` и `RecipeProgress`. А каждый тик MachineSystem требует чтения тех же компонентов. В результате вместо одного in-process tick (наносекунды) получаем сетевой roundtrip (миллисекунды) **каждый кадр**.

> **Вывод**: ECS в игровом контексте не режется на два сервиса без потери производительности. Это не баг архитектуры, а свойство домена — все объекты мира связаны.

## Решение: threading model внутри одного процесса

Вместо внешнего split'а — внутреннее разделение ответственности, без RPC:

```
┌───────────────────────────────────────────────────────────────────┐
│                    SimulationCore (один процесс)                  │
│                                                                   │
│  ┌──────────────┐  ┌──────────────────────┐  ┌─────────────────┐  │
│  │  IO Thread   │  │    Main Thread       │  │  Async Workers  │  │
│  │  (asio)      │  │    (20Hz tick)       │  │  (thread pool)  │  │
│  │              │  │                      │  │                 │  │
│  │  ─ recv Msg  │  │  ECS systems:        │  │  ─ GridMatcher  │  │
│  │  ─ pub Event │  │  ├ MachineSystem     │  │  ─ Recipe lookup│  │
│  │  ─ RPC call  │  │  ├ GeneratorSystem   │  │  ─ MetaDB write │  │
│  │              │  │  ├ BoilerSystem      │  │  ─ Heavy Валид. │  │
│  │  Queue →     │  │  ├ HeatTransfer      │  │                 │  │
│  │  Main thread │  │  └ CraftExecution    │  │  ─── callback ─►│  │
│  └──────┬───────┘  │                      │  │       Main      │  │
│         │          │  ─ action_dispatch() │  └─────────────────┘  │
│         │          │  ─ inv_handler()     │                       │
│         ▼          │                      │                       │
│  ┌────────────┐    └──────────────────────┘                       │
│  │work_queue_ │                                                   │
│  │(SPSC queue)│                                                   │
│  └────────────┘                                                   │
└───────────────────────────────────────────────────────────────────┘
```

### Как это работает

1. **IO Thread**: Принимает сообщения из сети (MessageRouter, RPC responses). Кладёт в `work_queue_` (lock-free SPSC queue). **Не трогает ECS registry.**

2. **Main Thread**: Один tick (50ms):
   - Drain `work_queue_` — превращает сырые сообщения в ECS-мутации
   - `onBlockChanged()` → поиск мультиблоков
   - Tick всех ECS-систем (MachineSystem, GeneratorSystem, ...)
   - `InventoryActionHandler` — мутация `InventoryContainer`
   - CPU-heavy операции (GridPatternMatcher) отправляет в Async Workers
   - Публикует события через IO Thread (неблокирующая очередь наружу)

3. **Async Workers**: Чистые функции, без доступа к ECS. Pattern matching, проверка рецептов, запись в MetaDB, валидация. Результат — callback обратно в Main Thread через `work_queue_`.

### Преимущества

- **Нет RPC** — вся ECS-логика в одном процессе, нулевая сетевая задержка
- **Tick не блокируется IO** — IO Thread асинхронный, tick не ждёт сеть
- **CPU-heavy не блокирует tick** — крафт/матчинг уходит в Async Workers
- **Одна source of truth** — ECS registry не дублируется
- **Ничего не надо менять в протоколе** — контракты с другими сервисами те же

### Компромиссы

- SimCore остаётся единственной точкой отказа для игровой логики (но это и так было)
- Сложнее отлаживать — три трейда, sharing data через SPSC queue
- Main Thread — single-threaded, не масштабируется на многоядерность для одного tick
- Если CPU-heavy задача в Async Worker не успевает за 50ms — tick ждёт (backpressure)

## Уже сделано: RecipeManager как stateless сервис

RecipeManager **уже реализован** как отдельный сервис (C++, порт :5130):

```
CraftRequest → RecipeManager (stateless)
  → проверка рецепта (чистая функция, idempotent)
  → возвращает {result, consumed, ticks}
  → SimCore сам мутирует ECS (InventoryContainer, RecipeProgress)
```

RecipeManager не хранит состояние — это просто БД рецептов + GridPatternMatcher.
Рестарт незаметен. Масштабируется горизонтально. Zero-cleanup.

Это **единственное**, что оторвалось безболезненно. Всё остальное (CraftExecution внутри SimCore,
InventoryAction, машины, энергия) мутирует ECS и не может быть вынесено без RPC-ада.

## Реальные боли (не enterprise)

### Крафт/инвентарь — saga без compensation

Цепочка крафта — 5 TCP roundtrip'ов через 4 сервиса:

```
Client → Gateway → SimCore → RecipeManager (проверка)
                                 → SimCore (мутация ECS)
                                 → MetaDB (запись)
                                 → Router → Gateway → Client
```

Проблема: каждый шаг может упасть, а **отката нет**:

| Сценарий | Что пошло не так | Состояние |
|----------|-----------------|-----------|
| SimCore проверил рецепт, упал до мутации ECS | RecipeManager сказал "ok, валидно", SimCore не применил | Рецепт не выполнен, но ресурсы не вернулись игроку — **ок** (потеря запроса, не данных) |
| SimCore применил в ECS, упал до публикации CraftResponse | Игрок увидел timeout. Ресурсы списаны, предмет не пришёл | **Потеря предмета** при retry (двойное списание) |
| SimCore мутировал ECS, отправил в MetaDB — MetaDB timeout | ECS — предмет есть (consumed + result), MetaDB — нет | После рестарта MetaDB не знает об изменении, inventory рассинхронизирован |
| SimCore отправил CraftResponse, упал до InventoryUpdate | Клиент увидел ответ, но InventoryUpdate не пришёл | Игрок видит неверный инвентарь до переподключения |

Нужна saga: либо `prepare → commit / rollback`, либо retry с идемпотентностью на каждом шаге. Пока нет ни того, ни другого.

### InventoryAction — распределённый монолит хуже монолита

Действие с инвентарём (MOVE/SPLIT/DROP/CRAFT) обрабатывается:

1. **Client** — UI рендер + локальный optimistic update
2. **Gateway** — релей сообщения
3. **SimCore** — `InventoryActionHandler` → ECS мутация
4. **MetaDB** — долгосрочное хранение (SQLite write)

Две source of truth: ECS (`InventoryContainer` компонент) и SQLite (MetaDB). Они синхронизируются по одному сообщению — без блокировки, без транзакции, без retry. При сбое между write в ECS и write в MetaDB — расходятся навсегда.

### Нет идемпотентности

Player нажал "скрафтить" дважды (double-click до прихода ответа):
- Два `CraftRequest` в SimCore
- RecipeManager выполнит проверку дважды (stateless — ок)
- SimCore спишет ресурсы дважды
- MetaDB запишет двойное изменение

На клиенте optimistic update уже показал результат. Через секунду инвентарь пуст.

### MessageRouter не восстанавливает подписчиков

Если MetaDB перезагрузилась (упала/поднялась):
- Она регистрируется заново как `metadb`
- Подписка на `player.inventory.actions` — активна
- **Но**: все сообщения, опубликованные пока MetaDB была в дауне — потеряны

Нет persistent queue, нет replay, нет `last_known_good` offset.

---

Всё это не убьёт MVP на 10 пользователей. Но первые полчаса с 20 игроками, активно крафтящими, отловят минимум половину этих сценариев.

Из них самое дешёвое по фиксу — идемпотентность на уровне CraftRequest (client-side request dedup + server-side idempotency key). Самое дорогое — saga с compensation поверх 5 сервисов.

## Итог

| Подход | Сложность | Производительность | Изоляция отказов |
|--------|-----------|-------------------|-------------------|
| 2 сервиса (ECS split) | Высокая | Низкая (RPC на каждый тик) | Средняя |
| 1 процесс, 3 треда | Средняя | Высокая | Низкая* |
| SimCore + Stateless Recipe | Низкая | Высокая | Средняя |

\* — но для игрового сервера это норм, точки отказа дублируются шардами по измерениям.

**Рекомендация**: Комбинация — threading model внутри SimCore + stateless RecipeManager снаружи. Остальное не трогать, пока не появится реальная нагрузка.
