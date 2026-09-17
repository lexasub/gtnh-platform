# Design: Multiplayer Foundation

## Context
Gateway (`src/services/gateway/`) — единственная точка входа внешних клиентов. Сегодня:
- singleton `client_ctrl_`/`client_bulk_` (`gateway.h:112-113`), accept заменяет старое
  соединение; второй клиент «убивает» первый;
- `client_player_id_` (gateway.cpp:106 — жёстко `1`, комментарий "matches client's hardcoded dev
  ID"), client шлёт `invState_.player_id = 1` (`GameClient.cpp:269`);
- все router→client ответы — `send_to_client_*` в единственное соединение; фильтрация
  source-исключения только по единственному id (gateway.cpp:407);
- `client_interest()` возвращает nullptr (TODO), `chunk.requests` уходят с `player_id=0`;
- `libgtnh-net::TcpServer::on_accept` даёт fd на каждое подключение — мультиплексирование на уровне
  библиотеки возможно, нужен только серверный state per connection.

Существующая спецификация `multiplayer-sync` уже описывает multi-client сценарии («player 2 видит
блок»), но код под них не заточен — требование фактически не выполняется при 2+ клиентах.

## Goals / Non-Goals
- Goals: N одновременных клиентов с изоляцией сессий; корректная адресация ответов; per-player
  интересы чанков; видимость других игроков; аккуратный join/leave/reconnect; базовые multi-client
  тесты и throughput baseline как точка отсчёта для кластера B.
- Non-Goals: авторизация/токены, шифрование/TLS, rate limiting, LZ4, backpressure, reconnect
  с resync-протоколом (это кластер B), публичный сервер, permissions/ban (кластер H).

## Decisions

### Decision 1: ClientSession — единица маршрутизации
`struct ClientSession { uint64_t session_id; std::shared_ptr<IoUringConnection> ctrl, bulk;
uint64_t player_id; std::string nickname; PlayerInterest interest; int32_t last_x/y/z; }`.
`IoUringGateway` держит `unordered_map<session_id, ClientSession>` + индексы
`player_id → session_id`, `bulk_fd → session_id`. Все `send_to_client_*` либо принимают target
(`player_id`/`session_id`), либо рассылают всем сессиям с фильтром source.

Альтернативы: (а) оставить singleton + «активный клиент» — не решает задачу; (б) отправлять всё в
роутер с per-player топиками — усложняет роутер и ломает существующие темы; отвергнуто: сессии в
gateway — минимальный шов, остальные сервисы не меняются.

### Decision 2: ctrl↔bulk pairing — по handshake-токену, не по IP
Первое сообщение на ctrl — `ClientHello{player_id(желаемый), nickname, bulk_token}`; на bulk — тот
же `bulk_token`. Gateway соединяет пару по токену (сгенерирован при accept на ctrl). Сессия живёт,
пока жив ctrl; bulk без ctrl-пары через таймаут — отбрасывается. Никаких предположений про
порядок подключения и одинаковый IP (VPN/разные хосты).

### Decision 3: Identity — dev-mode, но сервер-каноничный
Клиент присылает желаемый id + ник; сервер подтверждает канонический id (при коллизии выдаёт
свободный и уведомляет). Ненулевые `player_id` в действиях валидируются против сессии: чужой id →
действие отбрасывается (доверенная сеть ≠ разрешён спуфинг). Это выводит жёсткий `player_id=1` из
GameClient и gateway.

### Decision 4: Addressed replies через request_id-корреляцию
Два пути адресации: (а) payload несёт `player_id` получателя (инвентарь, квесты, позиции) —
шлём в его сессию; (б) ответы RPC-стиля несут `request_id` — gateway хранит
`request_id → session_id` на время запроса (TTL на garbage collect). Коллизия request_id между
клиентами исключается: корреляция per-session (ключ `session_id|request_id`), а не глобальный
счётчик. `world.blocks.changed`: `source_player_id` != 0 → не слать source, слать остальным
подписанным сессиям (текущее поведение сохраняется для N=1).

### Decision 5: Per-session interest
`PlayerInterest` переезжает в `ClientSession`; `chunk.requests` всегда с `player_id` сессии;
`player.position.load` → в сессию владельца. Gateway фильтрует `world.chunk.loaded.compressed` по
интересу каждой сессии (пока — куб-радиус, как сейчас); полный interest aggregator остаётся
отдельным deferred change'ом.

### Decision 6: Visibility — минимальный релей
Другие игроки видны как: (1) чужие `BlockChangedEvent` релеятся всем в интересах; (2) позиционные
`EntitySnapshot` события публикуются симуляцией на основе `player.joined`/позиций, gateway
маршрутизирует по интересу. Без интерполяции/предсказания (это дальше).

### Decision 7: Тесты — headless multi-client harness
`test/integration/`: два `gateway_cli`-подобных клиента на живом gatewayd: параллельные
handshake, уникальные id, изоляция ответов на одинаковый request_id, видимость чужого блока,
дисконнект одного не роняет второго. Throughput baseline: `gateway_cli` N=2..8, замер msg/s в
лог/отчёт, без gate (точка отсчёта для B).

## Risks / Trade-offs
- **Риск: перегрев gateway при фанауте.** Сейчас — один сокет; станет — 8. Mitigation: фанаут
  итерируется по сессиям под mutex, zero-copy shared_ptr фреймов (уже так), per-session очередь не
  нужна до кластера B (backpressure).
- **Риск: race между bulk-attach и ctrl-drop.** Mitigation: session state machine
  (AWAIT_BULK → ACTIVE → CLOSED) + `session_gen_` (уже есть в коде) расширить на сессии.
- **Риск: скрытые singleton-зависимости вне gateway** (например, симуляция может рассчитывать на
  один источник действий). Mitigation: контрактные тесты на 2+ клиентов в первом же этапе задач.
- **Trade-off: dev-handshake без auth.** Принято осознанно (доверенная сеть); закрывается в
  кластере B (auth/handshake: токен, версия протокола).

## Migration Plan
1. Gateway: ввести `ClientSession`, сохранить поведение N=1 (текущие тесты зелёные).
2. Протокол: `PlayerJoined`/`PlayerLeft` + nickname (backward-compatible поле).
3. Handshake + addressed sends; клиент переходит на серверный id.
4. Per-session interest + chunk.requests player_id.
5. Multi-client harness + throughput baseline.
6. ROADMAP A.1 + закрытие пункта «Gateway single-client».
Rollback: изменения gateway локализованы; N=1 поведение сохраняется при пустой map → singleton-путь
не удаляется до полного перехода.

## Open Questions
- Формат `EntitySnapshot` для позиций игроков: переиспользовать существующий или добавить
  `PlayerPosSnapshot`? Решить при имплементации шага visibility.
- Нужен ли лимит max_sessions (config)? По умолчанию 8.
