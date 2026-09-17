# Change: Multiplayer Foundation (trusted LAN/VPN, 2–8 players)

## Why
Gateway хранит ровно одно клиентское соединение (`client_ctrl_`/`client_bulk_` — singleton) и одно
поле `client_player_id_` (жёстко `= 1` и в gateway.cpp:106, и в GameClient.cpp:269). Все ответы
(`player.inventory.update`, `sim.craft.response`, `quest.*`, `player.actions.ack`, …) шлются
единственному клиенту. ROADMAP ставит мультиплеер перед этапом B (сетевой стек v2): чтобы честно
замерять пропускную способность и rate limiting, нужно сначала несколько одновременных клиентов.
Сегодня второй подключившийся клиент молча выкинет первого (accept заменяет соединение).

Scope согласован с пользователем: **доверенная сеть (LAN/VPN), кооп, 2–8 игроков**. Это фундамент,
не финальный публичный сервер: без аккаунтов/токенов, без шифрования. Но доверие сети не отменяет
технических инвариантов: каждое сообщение обязано быть привязано к сессии, ctrl↔bulk — соединены
явно, ответы — адресованы, интересы чанков — индивидуальны.

## What Changes
- **Gateway: сессии вместо singleton.** `client_ctrl_`/`client_bulk_` → map сессий
  (`session_id → ClientSession{ctrl_conn, bulk_conn, player_id, PlayerInterest, last_pos}`). Accept
  создаёт сессию, соединяя ctrl- и bulk-подключения в пару; дисконнект закрывает сессию целиком.
- **Identity: handshake по-минимуму.** Клиент шлёт ник + выбранный player_id (dev-режим, доверенная
  сеть); Gateway выдаёт канонический id, разрешая коллизии, и публикует `player.joined` с ником.
  Жёсткий `player_id = 1` в GameClient уходит; id выдаётся сервером.
- **Addressed replies.** Все gateway→client публикации шлются в сессию игрока-получателя, а не
  «текущему клиенту»: инвентарь/квесты/крафт/машинные ответы — по `player_id` из payload или
  `request_id → (session, player)` корреляции; `source_player_id` в `world.blocks.changed` —
  исключение отправителя, всем остальным — релей (перестаём зависеть от единственного
  `client_player_id_`).
- **Per-session chunk interest.** `client_interest()` nullptr → интерес на каждую сессию
  (`PlayerInterest` per session); `chunk.requests` атрибутируются player_id (сейчас шлются с
  `player_id=0`, gateway.cpp:488/491); `player.position.load` маршрутизируется в сессию игрока.
- **Remote player visibility.** Действия и позиция игрока N релеятся остальным как
  `EntitySnapshot`/`PlayerAction`-события (по договорённости — минимальный вариант: ретрансляция
  `BlockUpdate` + позиционных снапшотов), чтобы 2+ клиента видели мир согласованно.
- **Disconnect/reconnect по-сессионно.** `player.left` публикуется на каждый дисконнект (сейчас —
  глобально для единственного клиента); позиция и инвентарь сохраняются/восстанавливаются per
  player (MetaDB-схема уже готова — таблицы keyed by player_id).
- **Bulk↔ctrl pairing и жизненный цикл.** Bulk-подключение без ctrl-пары отклоняется; нет «замены»
  старой сессии новой (сейчас `bulk_server_.on_accept` свапает `client_bulk_`).
- **Multi-client тесты + базовый throughput baseline.** Headless-тесты: 2+ клиентов, независимость
  ответов, изоляция request_id, видимость чужих действий; замер msg/s на N клиентов как база для
  кластера B.
- **ROADMAP.** Новый раздел A.1 (мультиплеер-фундамент) в бэклоге перед B; пункт в «Известных
  проблемах» (Gateway single-client) закрывается этим change'ом.

## Impact
- Affected specs: `multiplayer-sync` (MODIFIED: Block Change Broadcast, Player Disconnect
  Announcement, Player Reconnect Restoration), новая `multiplayer-sessions` (ADDED: сессии,
  identity, addressed replies, per-session interest, visibility, lifecycle, тесты).
- Affected code:
  - `src/services/gateway/gateway.h/.cpp` — сессии, handshake, addressed send, per-session interest;
  - `src/services/game_client/GameClient.cpp` (`invState_.player_id = 1`), `NetClient.*` —
    handshake, серверный id;
  - `src/services/game_client/Cache/ChunkLoadManager.cpp` — запросы чанков с player_id сессии;
  - `src/services/simulation_core/` — только подписка на multi-player события (владение ECS не
    меняется; `ContainerSession` уже per-player);
  - `src/services/meta_db/` — приём ника в `player.joined` (схема БД не меняется);
  - `src/protocol/core.fbs` — `PlayerJoined`/`PlayerLeft` + ник (обратная совместимость: новое
    поле, не ломает старые фреймы);
  - `test/integration/`, `tools/gateway_cli/` — multi-client harness.
- **Non-Goals (в этом change):** LZ4/rate limiting/backpressure/reconnect-протокол (кластер B),
  chunk-interest-aggregator (deferred, про контент-регистрации сервисов), auth-токены/шифрование,
  публичный интернет-сервер, мобы/anti-cheat/permissions (кластер H).

## Constraints (from user)
- Доверенная LAN/VPN сеть; кооп; ориентир 2–8 одновременных игроков.
- Только proposal на этом шаге: реализация стартует после одобрения.
