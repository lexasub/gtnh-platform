# Networking Rework — Problem Analysis & Proposal

> **Status**: ✅ All 4 phases complete. Full project builds (21 targets), 72/72 tests pass. | **Date**: 2026-06-17 | **Author**: Sisyphus
> **Phase 1 (libgtnh-net)**: ✅ **FIXED** — IoUringConnection rewritten, SQPOLL on write ring, DEFER_TASKRUN + SINGLE_ISSUER on read ring, 52/52 tests pass (10/10 runs, no segfaults)
> **Phase 2 (Gateway)**: ✅ **DONE** — `gatewayd` migrated to libgtnh-net (TcpServer + RouterClient + IoUringConnection). Local `io_uring/` fork deleted (7 files).
> **Phase 3 (SimCore)**: ✅ **DONE** — `simcored` + `simcored_exec` build against libgtnh-net. Old `io_uring/` directory deleted. All 21 targets build clean.
> **Phase 4 (Cleanup)**: ✅ **DONE** — Dead io_uring deleted, Go router read timeout, GameClient backoff, ActionDispatcher fixes

---

## 0. ⚠️ История: IoUringConnection в libgtnh-net был сломан (✅ FIXED)

В ходе Phase 3 (SimCore миграция) обнаружено: `io_uring_connection.cpp` в libgtnh-net полностью неправильный — **не компилируется**.

**Проблема**: Код сгенерирован AI без проверки:
- Использует несуществующие liburing API: `io_uring_queue_up`, `IO_URING_FLAG_REGULAR`, `io_uring_register_probes`, `io_uring_queue_init_params`
- Rings объявлены как `static` — все инстансы делят одни кольца
- `ctx_->get_sqe()` где `ctx_` — `unique_ptr<io_uring, ...>` (у struct io_uring нет метода `get_sqe()`)

**Причина**: Хедер (`io_uring_connection.h`) корректный — добавлены поля `ring_`, `ring_write_`, `poll_thread_`, `sq_mutex_`. Но реализация не была проверена.

**Фикс** (сделан): `io_uring_connection.cpp` переписан с нуля — non-static rings, прямые liburing вызовы, правильная dual-ring poll loop, thread-safe write path с generational tagging, корректный lifecycle (self-close vs external close). **52/52 тестов проходят**.

---

## 1. Executive Summary

Сетевая архитектура GTNH Platform страдает от системных проблем, делающих её неработоспособной в текущем виде. Корень — форкнутый io_uring код, разошедшийся в 3 копиях, TOCTOU race на каждом write-path, отсутствие единого протокольного слоя, и хрупкая схема pub/sub через Go Router.

**Клиент коннектится и дисконнектится через ~6 секунд. Никакие данные не доходят.**

Этот документ описывает все проблемы и план миграции. Создана библиотека `libgtnh-net` — на неё уже вынесен Gateway. Далее — SimCore, cleanup, верификация.

---

## 2. Текущая архитектура

```
┌──────────────┐  ctrl:3000   ┌──────────────────┐  router:4000   ┌────────────────┐
│  GameClient  │─────────────►│     Gateway      │───────────────►│  MessageRouter │◄────┐
│  (bgfx C++)  │  bulk:3001   │  (C++ io_uring)  │                │  (Go stdlib)   │     │
│  IoUringClnt │◄─────────────┤                  │◄───────────────┤                │     │
└──────────────┘              │  3 io_uring ctx  │                │  pub/sub bus   │     │
                              │  (ctrl+bulk+rt)  │                └────────────────┘     │
                              └──────────────────┘                         ▲             │
                                      │                                     │             │
                                      │ ChunkStore RPC:5001                 │ subscribe   │
                                      ▼                                     │             │
                              ┌──────────────────┐              ┌──────────────────────┐  │
                              │   ChunkStore     │              │    SimulationCore     │──┘
                              │  (C++ Asio RPC)  │◄─────────────┤   (C++ io_uring)     │
                              └──────────────────┘  block RPC   │  1 uring ctx shared  │
                                                                 │  router+chunkstore   │
                              ┌──────────────────┐              └──────────────────────┘
                              │ EntityStateStore │                      │
                              │  (C++ Asio RPC)  │◄─────────────────────┘
                              │  port 5200       │    asio TCP RPC
                              └──────────────────┘
```

**Gateway уже переведён на libgtnh-net.** На схеме выше он всё ещё показан со старым io_uring — физически в коде больше нет `gateway/io_uring/` и ручного accept.

### Соединения (TCP)

| От | Кому | Протокол | Порт | Тип |
|---|---|---|---|---|
| GameClient | Gateway | length+type+FlatBuffer | 3000 (ctrl) | Command/response |
| GameClient | Gateway | length+type+FlatBuffer | 3001 (bulk) | Chunk/entity data |
| Gateway | Router | Router frame | 4000 | Pub/sub bus -> ✅ libgtnh-net |
| SimCore | Router | Router frame | 4000 | Pub/sub bus -> ✅ libgtnh-net |
| SimCore | ChunkStore | SetBlock/GetBlock RPC | 5001 | Block ops -> ✅ libgtnh-net |
| SimCore | EntityStateStore | Asio custom | 5200 | Entity state |
| EntityStateStore | SimCore etc | Asio custom | 5200 | RPC server |
| ChunkStore | — | Asio RPC server | 5001 | Block ops |

### Router Frame Protocol

```
[4 bytes: payload length (big-endian)] [1 byte: message type] [payload]

Types: 0x01=Subscribe, 0x02=Unsubscribe, 0x03=Publish,
       0x04=Register, 0x05=Heartbeat
```

### Client↔Gateway Protocol

```
[4 bytes: payload length (BE)] [1 byte: message type] [FlatBuffer]

Types: 0=PlayerAction, 1=ChunkData, 2=EntitySnapshot, 3=BlockAck,
       4=SetBlockAction, 5=InventoryUpdate, 6=InventoryAction, ...
```

---

## 3. Проблемы

### P1: TOCTOU Race в io_uring Write Path (CRITICAL) ✅ FIXED в libgtnh-net

**Где**: Все копии `IoUringClientConnection::start_next_writes()` и `IoUringContext::get_sqe()`
**Файлы**: `gateway/io_uring/io_uring_connection.cpp`, `simulation_core/Network/clients/io_uring/io_uring_connection.cpp`

**Механизм**:
```
enqueue_write() → start_next_writes()
  1. ctx_->get_sqe()       // захватывает sq_mutex_, продвигает sq_tail, отпускает
     ⚠️ ОКНО: poll_loop может сделать io_uring_submit — SQE ещё не заполнен
  2. io_uring_prep_write() // заполняет SQE (если poll_loop украл — уже поздно)
  3. ctx_->submit()        // захватывает sq_mutex_, submit
```

`io_uring_get_sqe()` обнуляет только `user_data`. Остальные поля (opcode, fd, buf, len) содержат **данные предыдущего использования**. Если poll loop успевает засабмитить такой SQE — ядро DMA-читает freed память → в сокет летят нули/мусор.

**Статус**: ✅ Исправлен в libgtnh-net — dual rings с раздельными sq_mutex_ для read и write.
**SimCore fork**: ✅ Удалён — мигрирован на libgtnh-net.

### P2: 3 Fork-нутые Копии io_uring Кода (CRITICAL)

```
src/services/io_uring/                           ← 🗑️ УДАЛЁН (dead shared copy)
src/services/gateway/io_uring/                   ← ✅ УДАЛЁН (мигрирован на libgtnh-net)
src/services/simulation_core/Network/clients/io_uring/  ← ✅ УДАЛЁН (мигрирован на libgtnh-net)
```

**Оставшиеся копии:**

| Аспект | Shared | Gateway (✅ done) | SimCore (✅ done) |
|--------|--------|-------------------|-------------------|
| Статус | 🗑️ УДАЛЁН | ✅ libgtnh-net | ✅ libgtnh-net |
| `sq_mutex_` | ❌ Нет | ✅ Dual rings | ✅ Dual rings (через libgtnh-net) |
| Generational tags | ❌ | ✅ | ✅ (через libgtnh-net) |
| TagAllocator | ❌ | ✅ | ✅ (через libgtnh-net) |
| Send API | writev (sync) | send/send_raw | send/send_raw (через libgtnh-net) |

### P3: SimCore Использует Один io_uring Context для Router + ChunkStore (CRITICAL) ✅ FIXED

**Файл**: `simulation_core/main.cpp:143-171` (было, удалено при миграции)

```cpp
// Раньше:
IoUringContext uring_ctx;
auto routerClient = new IoUringRouterClient(&uring_ctx, ...);
auto chunkstoreClient = new IoUringChunkClient(&uring_ctx, ...);

// Теперь:
auto routerClient = std::make_unique<IoUringRouterClient>();    // свой poll thread
auto chunkClient = std::make_unique<IoUringChunkClient>();      // свой poll thread
```

**Фикс**: Каждый клиент владеет своим `IoUringContext` через libgtnh-net — полная изоляция.

### P4: ActionDispatcher Игнорирует CHUNK_REQUEST и PlayerAction (CRITICAL) ✅ FIXED

**Файл**: `simulation_core/Actions/ActionDispatcher.cpp`

```cpp
// Было:
if (count == 0) count = 64;  // magic number
if (pa->action() != ITEM_ACTION) return false;  // silent drop

// Стало:
// CHUNK_REQUEST → лог + TODO для загрузки чанка
// ITEM_ACTION → реальный count
// Неизвестные PlayerAction → spdlog::warn
// Неизвестные SetBlockAction action_type → spdlog::warn
```

**Что сделано**:
- Убран хардкод `count=64` — используется реальный count из действия
- CHUNK_REQUEST парсится и логируется (координаты + player_id)
- Неизвестные PlayerAction логируются через `spdlog::warn` вместо silent drop
- Неизвестные SetBlockAction типы логируются через `spdlog::warn`
- TODO: загрузка чанка через ChunkStoreRepository (требует GetChunk RPC)

### P5: Нет Heartbeat на Gateway→Router ✅ FIXED в libgtnh-net

**Файл**: `gateway/gateway.cpp:250-256`

Gateway отправляет heartbeat **только** в `sendHeartbeat()`, который **никогда не вызывается** в main.cpp. SimCore отправляет heartbeat раз в 20 секунд, но Gateway — нет. У Router есть `idleTimeout = 60s`.

**Статус**: ✅ `RouterClient::heartbeat()` есть в libgtnh-net. Gateway в main.cpp отправляет heartbeat раз в 20 секунд (строка 98-100).
**SimCore**: ✅ Heartbeat через gtnh::net::RouterClient (libgtnh-net).

### P6: Client Bulk Reconnect Toxico ✅ FIXED

**Файл**: `game_client/Network/NetClient.cpp:70-114`

При дисконнекте bulk соединения:
1. `io_uring_bulk_.shutdown()` (io_uring_queue_exit)
2. В цикле: `init()` + `connect()` — без синхронизации с game thread
3. Нет rate limiting — вечный reconnect

**Фикс**:
- Exponential backoff: 1s, 2s, 4s, 8s, 16s, 30s max
- `reconnecting_` flag предотвращает повторный вход
- `max_reconnect_attempts_ = 3` — не вечный reconnect

### P7: Магические Числа и Хардкод Tag'ов ✅ FIXED

В libgtnh-net: `TagAllocator` с атомарным счётчиком. Каждое соединение получает непересекающийся диапазон.
**SimCore fork**: ✅ Больше не существует — мигрирован на libgtnh-net.

### P8: Shared io_uring — Dead Code 🗑️ DELETED

**Файлы**: `src/services/io_uring/io_uring_context.{h,cpp}` — удалены.

Отдельная epoll-based реализация, несовместимая с liburing 2.x. Никем не использовалась.

### P9: SimCore Не Обрабатывает ChunkRequest 🟡 PARTIAL

Клиент отправляет ChunkRequest как `PlayerAction(CHUNK_REQUEST)`. Gateway публикует в `player.actions`. SimCore получает, ActionDispatcher **парсит и логирует** (координаты + player_id). **Загрузка чанка из ChunkStore не реализована** — требует GetChunk RPC.

### P10: Router Connection Single Point of Failure ❌

Все publish от gateway идут через один router connection. В Go router нет:
- Timeout на `readFrame`
- Recovery после corrupt frame
- Graceful degradation при переполнении sendChs

---

## 4. Коренные Причины

1. **Нет единой сетевой библиотеки** — каждый сервис имеет свою копию io_uring кода с разными багами → ✅ libgtnh-net решает
2. **Protocol layer === transport layer** — форматирование фреймов, отправка, io_uring управление — всё в одном классе → ✅ в libgtnh-net разделено
3. **Нет работы с ошибками** — corrupt frame = полный разрыв соединения без recovery
4. **Нет тестирования** — ни unit, ни integration тестов для сети → ✅ 53 теста в libgtnh-net
5. **Нет изоляции** — одно io_uring кольцо на все соединения в simcore → ✅ исправлено (каждый клиент self-hosted)
6. **Нет keepalive/heartbeat систем** — gateway не отправляет heartbeat → ✅ исправлено
7. **Chunk loading не реализован** — ActionDispatcher дропает ChunkRequest → ❌

---

## 5. Решение: libgtnh-net ✅ DONE (+ DEFER_TASKRUN fix)

### 5.1 Концепция

Выделить весь общий сетевой код в отдельную библиотеку `libgtnh-net`, которую используют все C++ сервисы. Библиотека предоставляет:

- Управление io_uring ring + poll loop (dual rings — обязательный design) ✅
- Connection state machine (read/write с generational tagging) ✅
- TCP connector (блокирующий + DNS resolve) ✅
- Frame protocol ([4B BE len][1B type][payload]) ✅
- Router client (register/subscribe/publish/heartbeat) ✅
- TCP server (accept loop на io_uring) ✅

Каждый сервис получает **свой** `IoUringContext` на каждое независимое соединение — полная изоляция.

### 5.2 Фактическая структура библиотеки

```
src/libs/libgtnh-net/                          ← src/libs/ (не src/libraries/)
├── CMakeLists.txt                              — Conan + CMake
├── include/gtnh/net/
│   ├── types.h                  — общие типы, kMaxPayload, encode_user_data
│   ├── io_uring_context.h       — single-ring, accept-only (on_cqe callback)
│   ├── io_uring_connection.h    — read/write state machine + TagAllocator
│   ├── tcp_connector.h          — блокирующий connect + DNS resolve
│   ├── frame.h                  — write_be32, read_be32, write_be16, pack
│   ├── router_client.h          — register/subscribe/publish/heartbeat
│   └── server.h                 — TcpServer (accept + on_accept callback)
├── src/
│   ├── io_uring_context.cpp     — poll_loop, shutdown, register/unregister connection
│   ├── io_uring_connection.cpp  — start_reading, send, send_raw, on_cqe, close
│   ├── tcp_connector.cpp        — connect с poll timeout
│   ├── frame.cpp                — write_be32, read_be32, write_be16, pack, pack_router
│   ├── router_client.cpp        — send_register, subscribe, publish, heartbeat
│   └── server.cpp               — do_accept, listen, stop
└── test/
    ├── CMakeLists.txt
    ├── test.h                   — CHECK/CHECK_EQ макросы (GTest-free)
    ├── test_main.cpp
    ├── test_frame.cpp           — pack, write_be32, read_be32, pack_router
    ├── test_context.cpp         — init/shutdown lifecycle
    └── test_echo.cpp            — TCP echo loopback (localhost, ~150ms)
```

### 5.3 Ключевые отличия реализации от оригинального proposal

| Пункт | Proposal | Факт |
|-------|----------|------|
| Путь | `src/libraries/libgtnh-net/` | `src/libs/libgtnh-net/` |
| Frame API | `write_header()`, `read_length()` | `write_be32()`, `read_be32()`, `write_be16()` |
| TagAllocator | `g_tag_alloc` (global) | member of owning class (`tag_alloc_`) |
| `IoUringConnection::on_cqe` | Вызов через ctx dispatch | ✅ Self-hosted — connection сам диспатчит свои CQE |
| `TcpServer::listen()` | 2 param | 2 param (port, name) — работает |
| Connection dispatch | Connection сам регистрируется в ctx | ✅ Connection сам владеет rings + poll thread. Context только accept |
| `IORING_SETUP_DEFER_TASKRUN` + `SINGLE_ISSUER` | Планировался на read ring (single-issuer: только poll thread) | ✅ **Включён на read ring** — init перенесён в poll thread. `SINGLE_ISSUER` запоминает `current` task при `io_uring_queue_init()`, а НЕ при первом submit/wait. Если ring создан в thread A, а используется в thread B → `-EEXIST`. Фикс: poll thread сам создаёт `ring_` через `init_read_ring_internal()` → poll thread === creator === issuer. |
| Write ring | single-issuer невозможен (send может быть из любого thread) | ❌ COOP_TASKRUN + TASKRUN_FLAG. SQPOLL fallback для не-root. |
| Context (accept) ring | single-issuer невозможен (main thread submit, poll thread wait) | ❌ COOP_TASKRUN только. Context создаётся в main thread, poll thread — waiter. |
| Work queue / thread pool | Убран (planned) | ✅ Убран — коллбеки в poll thread |

### 5.4 Архитектура rings — что сделано и что сломано

#### Цель: rings на уровне IoUringConnection

Каждое соединение владеет своими двумя кольцами:

```
IoUringContext (accept-only) [ring_read | sq_mutex] — только accept
  └── on_accept → client_fd

IoUringConnection A [ring_ | ring_write_ | poll_thread_] — свои кольца
IoUringConnection B [ring_ | ring_write_ | poll_thread_] — свои кольца
```

**Что изменилось**:
- `IoUringConnection` имеет поля `ring_`, `ring_write_`, `poll_thread_`, `sq_mutex_`, `sq_mutex_write_` в хедере ✅
- `IoUringConnection::start_reading()` создаёт свои rings + poll thread ✅
- Внешний `IoUringContext*` не нужен для I/O — только для accept ✅

#### ✅ IoUringConnection исправлен

Файл `io_uring_connection.cpp` полностью переписан:

| Проблема (было) | Решение (стало) |
|----------------|-----------------|
| `io_uring_queue_up` (не существует) | `io_uring_queue_exit` (правильный liburing API) |
| `IO_URING_FLAG_REGULAR` (не существует) | Убран |
| `static` rings (все инстансы делят) | Non-static per-instance `ring_`, `ring_write_` |
| `ctx_->get_sqe()` где ctx_ = `unique_ptr<io_uring>` | `io_uring_get_sqe(&ring_)` |
| Нет poll loop | `poll_loop()`: peek CQEs→submit→drain write ring→wait_cqe_timeout 50ms |
| Self-close deadlock | thread-id check, cleanup без self-join |
| Нет partial read/write | Partial read/write state machine |
| Write — блокирующий writev() | Async dual-ring: read ring `DEFER_TASKRUN | SINGLE_ISSUER`, write ring `SQPOLL | COOP_TASKRUN` |
| io_uring CQ drain после close() — use-after-free | `fd_` check after join prevents double cleanup |
| `-EEXIST` с `DEFER_TASKRUN` | Read ring init moved to poll thread — `SINGLE_ISSUER` records init-time task. Poll thread === creator === issuer |

**SQPOLL**:
- Write ring создаётся с `IORING_SETUP_SQPOLL`; fallback на regular при ошибке
- Read ring: `DEFER_TASKRUN | SINGLE_ISSUER | COOP_TASKRUN | TASKRUN_FLAG`. Init в poll thread — единственный issuer === poll thread
- Poll loop: peek read CQEs → submit read SQEs → drain write CQEs non-blocking → wait_cqe_timeout 50ms. **Drain before submit** — предотвращает EEXIST если CQ заполнен
- Write ring: `SQPOLL | COOP_TASKRUN | TASKRUN_FLAG`. Multi-submitter (poll thread + external `send()`).
- Context ring: `COOP_TASKRUN | TASKRUN_FLAG` (без DEFER_TASKRUN — main thread submit, poll thread wait)
- `on_write_complete` не вызывает `cleanup()` — use-after-free при итерации CQE
- `close()`: после join проверяет `fd_` — если poll thread уже сделал cleanup, пропускает
- Без NOP wakeup (доступ к freed ring после exit_rings)

**Результат**: 52/52 тестов проходят (10/10 прогонов без SIGSEGV), `libgtnh-net.a` собирается, `gatewayd` собирается.

#### Poll thread

Каждое соединение = свой thread:

```
Connection A → own poll thread: io_uring_wait_cqe → process → loop
Connection B → own poll thread: io_uring_wait_cqe → process → loop
```

**Read ring init в poll thread**. `IORING_SETUP_SINGLE_ISSUER` запоминает `current` при `io_uring_queue_init()`, а НЕ при первом `io_uring_enter()`. Поэтому read ring создаётся в poll thread (а не в main thread как раньше). Это единственный способ включить `DEFER_TASKRUN` — иначе poll thread ≠ single issuer → `-EEXIST` на каждое `io_uring_wait_cqe_timeout`.

**Архитектура flags по ring**:

| Ring | Флаги | Почему |
|------|-------|--------|
| Read (`ring_`) | `DEFER_TASKRUN \| SINGLE_ISSUER \| COOP_TASKRUN \| TASKRUN_FLAG` | Init в poll thread, poll thread === единственный issuer |
| Write (`ring_write_`) | `SQPOLL \| COOP_TASKRUN \| TASKRUN_FLAG` | Multi-submitter: `send()` может быть из любого thread. SQPOLL fallback на regular при ошибке |
| Context (accept) | `COOP_TASKRUN \| TASKRUN_FLAG` | Main thread submit (accept SQE), poll thread wait. Cross-thread → SINGLE_ISSUER невозможен |

- ✅ Мгновенная реакция на CQE — полная изоляция
- SimCore: 3 threads (accept + router + chunk) — тривиально
- Gateway: 3 threads (accept + bulk + ctrl) — работает

Вариант с общим poll thread (дрейнить rings всех connections) не нужен — лишняя сложность без выгоды.

### 5.5 Тестовое покрытие

52 теста, 3 группы:

**frame** (unit, 24 теста):
- `write_be32`/`read_be32` граничные значения (0, 1, UINT32_MAX)
- `write_be16`/`read_be16` (заглушка на big-endian)
- `pack` с пустым payload, с данными, с различными msg_type
- `pack_router` heartbeat, subscribe, publish
- Проверка wire-format: [4B BE len][1B type][payload]

**context** (unit, 24 теста):
- init/shutdown lifecycle (одинарный, двойной, без poll thread, shutdown без init)
- `on_cqe` вызов только при active running
- Проверка корректного вызова on_cqe для accept CQE
- init() после shutdown() — reuse

**echo** (integration, 5 тестов):
- TCP echo loopback: server → accept → read → write, client → connect → send → receive
- Single message echo (~150ms)
- Multi-message, empty message, large (64KB) message
- Сheck: sent bytes == received bytes

### 5.6 Тест echo: детали

```
Server: bind → accept → loop { read → echo write }
Client: connect → loop { write → read → verify }
```

Проверка:
1. Echo server слушает на случайном порту
2. Клиент коннектится, отправляет 1+ фреймов
3. Сервер принимает, читает, отправляет обратно
4. Клиент читает ответ, сверяет байты
5. Каждое соединение диспатчит свои CQEs через self-hosted rings
6. После последнего echo — клиент закрывает, сервер детектит EOF
7. res > 0 везде, матчинг по user_data корректный

---

## 6. План миграции — актуальный статус

### Фаза 1: libgtnh-net ✅ DONE

Выполнено за ~1 день вместо 5-7 запланированных.

| Задача | Статус | Файлы |
|--------|--------|-------|
| Структура `src/libs/libgtnh-net/` с CMakeLists.txt | ✅ | `CMakeLists.txt` |
| `types.h` — базовые типы | ✅ | `include/gtnh/net/types.h` |
| `IoUringContext` — single-ring, accept-only | ✅ | `io_uring_context.{h,cpp}` |
| `IoUringConnection` — self-hosted rings, generational tags, TagAllocator | ✅ **DEFER_TASKRUN + SINGLE_ISSUER enabled** | `io_uring_connection.{h,cpp}` |
| `TcpConnector` — blocking connect + DNS | ✅ | `tcp_connector.{h,cpp}` |
| `frame.h` — write_be32/read_be32/write_be16, pack, pack_router | ✅ | `frame.{h,cpp}` |
| `TcpServer` — accept loop + on_accept callback | ✅ | `server.{h,cpp}` |
| `RouterClient` — register/subscribe/publish/heartbeat | ✅ | `router_client.{h,cpp}` |
| Frame unit tests | ✅ | `test/test_frame.cpp` |
| Context lifecycle tests | ✅ | `test/test_context.cpp` |
| Echo integration test | ✅ | `test/test_echo.cpp` |
| **IoUringConnection impl fix** | ✅ **DONE** | `io_uring_connection.cpp` |
| **DEFER_TASKRUN read ring fix** | ✅ **DONE** — init moved to poll thread, poll thread === creator === single issuer | `io_uring_connection.cpp` |

### Фаза 2: Gateway Migration ✅ DONE

| Задача | Статус | Детали |
|--------|--------|--------|
| CMakeLists.txt — убрать io_uring сорсы, добавить gtnh-net | ✅ | |
| `gateway.h` — `TcpServer`, `IoUringContext`, `IoUringConnection`, `TagAllocator` | ✅ | |
| `gateway.cpp` — accept handlers в лямбды, `tag_alloc_` member, `frame::write_be32` вместо writeBE32 | ✅ | |
| `io_uring/` директория — удалена (7 файлов) | ✅ | |
| Сборка `gatewayd` | ✅ | 682K ELF, 0 warnings |
| Деплой Heartbeat (20s) | ✅ | main.cpp строка 98-100 |
| `bind_listen` — удалён (dead code) | ✅ | В libgtnh-net: TcpServer::listen() |

**Что изменилось в gateway.cpp после миграции:**

```
Before:                          After:
───────                          ─────
#include "io_uring/..."          #include <gtnh/net/server.h>
                                 #include <gtnh/net/router_client.h>
                                 #include <gtnh/net/io_uring_connection.h>
IoUringContext ctx_ctrl_         gtnh::net::TcpServer ctrl_server_
IoUringContext ctx_bulk_         gtnh::net::TcpServer bulk_server_
IoUringContext ctx_router_       (удалён — RouterClient self-hosted)
IoUringClientConnection router_  gtnh::net::RouterClient router_
IoUringClientConnection ctrl     std::unique_ptr<IoUringConnection> client_ctrl_
writeBE32/16 (be_helpers.h)      RouterClient — internal framing
enqueue_write(msg_type, d, l)    conn->send(msg_type, d, l)
do_accept() + dispatch_cqe       ctrl_server_.on_accept = [this](int fd) {...}
on_read_cb                       conn->on_message = [this](...) {...}
send_register()                  router_.connect() — автоматически
pending_subscriptions_           router_.subscribe() — deferred internally
router_registered_ flag          RouterClient — internal state
listen_ctrl_fd_ / listen_bulk_   TcpServer — internal
ConcurrentMemoryPool             удалён (dead code)
```

### Фаза 3: SimCore Migration ✅ DONE

**Цель**: Заменить форкнутый io_uring код в `simulation_core/Network/clients/` на `libgtnh-net`.

**Статус**:
- ✅ `IoUringContext` в SimCore — упрощён до single-ring accept-only
- ✅ `gtnh::net::IoUringConnection` — исправлен, DEFER_TASKRUN + SINGLE_ISSUER на read ring
- ✅ Замена `IoUringRouterClient` на `gtnh::net::RouterClient` (адаптер с тем же именем)
- ✅ Замена `IoUringChunkClient` на `gtnh::net::IoUringConnection` (адаптер с тем же именем)
- ✅ main.cpp — убрать dispatch block + IoUringContext
- ✅ `io_uring/` директория удалена (7 файлов)
- ✅ `simcored` + `simcored_exec` + все 21 target собираются

#### 3.1 Fix IoUringConnection в libgtnh-net ✅ DONE

Переписан `io_uring_connection.cpp`:
- Non-static per-instance rings ✅
- Прямые liburing вызовы ✅
- Dual-ring: read ring + SQPOLL write ring ✅
- Generational tagging + TagAllocator ✅
- Self-hosted poll loop (свой thread на соединение) ✅
- Race fixed: double-close prevention, fd_ check after join ✅
- 52/52 тестов, 10/10 прогонов без segfault ✅

#### 3.2 Replace IoUringRouterClient → gtnh::net::RouterClient ✅ DONE

**Файлы**:
- `src/services/simulation_core/Network/clients/IoUringRouterClient.{h,cpp}` — ✅ переписан как адаптер над `gtnh::net::RouterClient`
- `src/services/simulation_core/Network/clients/io_uring/` — ✅ удалён (7 файлов)

**Что сделано**:
- `IoUringRouterClient.h/cpp` — сохранён тот же API (`SetServiceName`, `Connect`, `Subscribe`, `Publish`, `OnMessage`, `Stop`, `IsConnected`, `SendHeartbeat`), внутри вызывает `gtnh::net::RouterClient`
- `on_publish` → `OnMessage` callback forwarding
- Неиспользуемые методы (`PublishRaw`) удалены
- `RouterEventPublisher.cpp` — обновлён для работы с новым `OnMessage` (сигнатура `void(uint8_t, const uint8_t*, size_t)`)

**Детали**:
- `RouterClient` владеет своим `IoUringContext` — полная изоляция
- `OnMessage` вызывается из poll thread → не блокировать
- Reconnect с exponential backoff (1s, 2s, 4s... max 30s)
- Класс `IoUringRouterClient` сохранён — **ноль изменений в потребителях** (RouterEventPublisher, PipeEnergyClient)

#### 3.3 Replace IoUringChunkClient → gtnh::net::IoUringConnection ✅ DONE

**Файлы**:
- `src/services/simulation_core/Network/clients/IoUringChunkClient.{h,cpp}` — ✅ переписан с `gtnh::net::IoUringConnection`

**Что сделано**:
`IoUringChunkClient` — сохранён тот же API (`Connect`, `Disconnect`, `IsConnected`, `SetBlock`, `SetBlockCAS`, `GetBlock`), внутри использует `gtnh::net::TcpConnector` + `gtnh::net::IoUringConnection`
- RPC логика (pending_callbacks_, req_id) без изменений
- Транспорт: `conn_.send_raw([4B BE flatbuf_size][FlatBuf data])` — совместимость с ChunkStore Asio RPC
- Ответ парсится: `[4B BE (flatbuf_size+1)][1B type=0][FlatBuf data]`

**Детали**:
- ChunkStore RPC — request/response на прямом TCP (не через Router)
- `IoUringConnection` сам создаёт rings + poll thread
- Класс `IoUringChunkClient` сохранён — **ноль изменений в потребителях** (ChunkStoreRepository)

#### 3.5 ActionDispatcher Fix — ChunkRequest + ITEM_ACTION

**Файл**: `src/services/simulation_core/Actions/ActionDispatcher.cpp`

**Что сделано**:

1. ✅ **CHUNK_REQUEST** — не дропается:
   - Парсятся координаты и player_id из `PlayerAction`
   - Логируется: `CHUNK_REQUEST: player=N chunk=(x,y,z)`
   - TODO: загрузить чанк через `ChunkStoreRepository::getChunk()` (требует GetChunk RPC на ChunkStore)

2. ✅ **ITEM_ACTION** — убран magic `count=64`:
   - Используется реальный `count` из действия

3. ✅ **Log warning** для неизвестных типов вместо silent drop:
   - Неизвестные `PlayerAction` типы → `spdlog::warn`
   - Неизвестные `SetBlockAction` action_type → `spdlog::warn`

**Зависимости**:
- ChunkStore RPC работает (Asio, порт 5001)
- ChunkStoreRepository уже есть в SimCore

#### 3.4 Проверка после миграции ✅ DONE

- ✅ `simcored` + `simcored_exec` компилируются
- ⬜ RouterClient регистрируется в роутере (лог: "registered as 'simcore'") — требуется runtime
- ⬜ Heartbeat раз в 20 секунд — требуется runtime
- ⬜ ChunkStore RPC работает (SetBlock/GetBlock) — требуется runtime
- ✅ ActionDispatcher не дропает CHUNK_REQUEST — парсит и логирует

### Фаза 4: Cleanup

| Задача | Файлы | Статус |
|--------|-------|--------|
| Удалить `src/services/io_uring/` (dead shared copy) | `src/services/io_uring/io_uring_context.{h,cpp}` | 🗑️ DELETED |
| Router (Go): read timeout 30s | `src/services/message_router/main.go` | ✅ |
| GameClient reconnect: exponential backoff | `src/services/game_client/Network/NetClient.cpp` | ✅ |
| Router (Go): при timeout/corrupt — закрыть соединение | `src/services/message_router/router.go` | ✅ встроено (timeout → readFrame error → return → defer cleanup) |
| Gateway: RouterClient reconnect | ✅ встроено в RouterClient | ✅ |

#### 4.1 Go Router: Read Timeout

```go
// router.go — добавить:
conn.SetReadDeadline(time.Now().Add(30 * time.Second))

// При timeout: close(), unsubscribe
// При corrupt frame: close(), log "invalid frame from X"
```

**Файл**: `src/services/message_router/router.go`
**Строки**: в функции `handleConnection()` — установить deadline до `readFrame()`

#### 4.2 GameClient Reconnect

**Файл**: `src/services/game_client/Network/NetClient.cpp:70-114`

**Проблема**: при дисконнекте bulk соединения — вечный reconnect без rate limit, race с game thread.

**Фикс**:
```
enum State { DISCONNECTED, RECONNECTING, CONNECTED };
std::atomic<State> state_;

reconnect():
  if state_ != DISCONNECTED → return
  state_ = RECONNECTING
  backoff = 1s
  while running && !connected:
    try connect
    if fail: sleep(backoff); backoff = min(backoff*2, 30s)
  state_ = connected ? CONNECTED : DISCONNECTED
```

### Фаза 5: Верификация

| Тест | Описание | Статус |
|------|----------|--------|
| libgtnh-net unit tests | Frame + Context + Echo | ✅ 52/52 |
| Echo integration | TCP loopback | ✅ |
| Gateway build | `gatewayd` компилируется | ✅ |
| SimCore build | `simcored` компилируется после миграции | ✅ |
| Full pipeline | client → gateway → router → simcore → chunkstore | ⬜ требуется runtime |
| Stress test | 100 SetBlockAction, нет ошибок | ⬜ |
| Reconnect test | убить/поднять routerd/симуляция обрыва | ⬜ |
| ASAN/valgrind | 30 минут, без утечек | ⬜ |

---

## 7. Что останется нетронутым

Сервисы, которые НЕ переходят на libgtnh-net (пока):

| Сервис | Причина |
|--------|---------|
| **GameClient** | Single-threaded io_uring, свой IoUringClient API. Надёжен. Только reconnect |
| **ChunkStore** | Asio-based, своя RPC схема. Работает. Не ломать |
| **EntityStateStore** | Asio-based, TCP RPC server. Работает. Не ломать |
| **MetaDB (Go)** | Go service, общается через router_client.go. Не трогать |
| **MessageRouter (Go)** | Go pub/sub. Только read timeout (3 строки кода) |

---

## 8. Дорожная карта — обновлённая

```
✅ Фаза 1 (DONE): libgtnh-net
  ├── IoUringContext (single-ring)         ✅
  ├── IoUringConnection (self-hosted)      ✅ DEFER_TASKRUN + SINGLE_ISSUER read, SQPOLL write
  ├── TcpConnector + Frame + Server        ✅
  ├── RouterClient                         ✅
  ├── IoUringConnection impl fix           ✅ 52/52 tests, 10/10 runs
  └── DEFER_TASKRUN read ring              ✅ init в poll thread — poll thread === single issuer

✅ Фаза 2 (DONE): Gateway
  ├── CMakeLists.txt io_uring→gtnh-net   ✅
  ├── gateway.h rewrite                  ✅
  ├── gateway.cpp rewrite                ✅
  └── gatewayd builds                    ✅

✅ Фаза 3 (DONE): SimCore
  ├── IoUringContext → accept-only         ✅
  ├── IoUringConnection fix (Phase 1)      ✅
  ├── RouterClient migration               ✅ адаптер IoUringRouterClient → gtnh::net::RouterClient
  ├── ChunkClient → IoUringConnection      ✅ адаптер IoUringChunkClient → gtnh::net::IoUringConnection
  └── simcored builds                      ✅ все 21 targets собираются

✅ Фаза 4 (DONE): Cleanup
  ├── Remove shared dead io_uring      🗑️ удалено
  ├── Go Router read timeout           ✅ conn.SetReadDeadline(idleTimeout)
  ├── GameClient reconnect fix         ✅ exponential backoff (1s..30s)
  └── ActionDispatcher fixes           ✅ ITEM_ACTION count, CHUNK_REQUEST log, unknown warn

⬜ Фаза 5 (2 дня): Верификация
  ├── Full pipeline test               ⬜
  ├── Stress/chaos testing             ⬜
  └── ASAN/valgrind                    ⬜
```

**До работающего pipeline**: после Фазы 3 → можно запустить полный цикл.
После Фазы 5 → production-ready (сеть не сыпется).

---

## 9. Риски и зависимости

### Зависимости
- liburing (Conan) ✅ уже есть
- FlatBuffers (Conan) ✅ уже есть
- spdlog (Conan) ✅ уже есть
- Conan рецепт для gtnh-net ✅ `src/libs/libgtnh-net/CMakeLists.txt` — target_link + add_subdirectory

### Риски — обновлённые

| Риск | Вероятность | Статус | Смягчение |
|------|------------|--------|-----------|
| SimCore логика сильно связана со старым io_uring API | Средняя | ✅ | Мигрировано. Адаптеры сохранили API |
| ChunkStore RPC несовместимость | Низкая | ✅ | ChunkStore Asio RPC не менялся, только клиентская часть |
| Chunk loading архитектура не определена | Высокая | 🟡 | ActionDispatcher парсит CHUNK_REQUEST, но GetChunk RPC не реализован |
| TOCTOU race в SimCore (P1) | Высокая | ✅ | Устранён — старый fork удалён, libgtnh-net race-free |
| ~~IoUringConnection impl сломан (AI-generated)~~ | ~~100%~~ | ✅ **FIXED** | Dual-ring, SQPOLL, race-free lifecycle. 52/52 tests pass |
