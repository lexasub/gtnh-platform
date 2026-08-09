# Tracing System

Мы используем Loki для сбора и трассировки отдельных запросов (block break/place, machine interaction).

**Это НЕ метрики производительности.** TRACE логи — это идентификаторы (request_id/tid), которые
позволяют связать hop'ы одного запроса через все сервисы. Время выполнения (dur_us) — 
побочная информация для прикидки где что тормозит, а не для мониторинга.

## Формат строки

```
[TRACE tid=N] svc op dur_us
```

Где:
- `tid` — request_id (число), связывает все шаги одного действия
- `svc` — сервис: `simcore`, `cas_cb`, `gateway`, `client`
- `op` — что произошло: `publish_ack`, `complete`, `publish_block_changed`,
  `relay_block_changed`, `skip_relay_to_source`, `recv_block_ack`, `recv_block_changed`
- `dur_us` — время в микросекундах (для справки)

## Откуда шлётся

TRACE_LOG макрос определён в:

| Файл | svc | op |
|------|-----|-----|
| `simulation_core/Actions/SetBlockCASHandler.cpp` | `cas_cb` | `complete` |
| `simulation_core/Network/RouterEventPublisher.cpp` | `simcore` | `publish_ack`, `publish_block_changed` |
| `gateway/gateway.cpp` | `gateway` | `relay_block_changed`, `skip_relay_to_source` |
| `game_client/Network/NetClient.cpp` | `client` | `recv_block_ack`, `recv_block_changed` |

## Инфраструктура (192.168.2.109)

| Компонент | Порт | Назначение |
|-----------|------|------------|
| Loki | `:13100` | HTTP API для запросов |
| tcp-bridge | `:1514` | TCP → Loki HTTP bridge (принимает nc, батчит) |
| Grafana | `:13000` | Дашборды (логин `admin` / `adminadmin`) |

Схема: сервисы → `nc loki 1514` → tcp-bridge (Go, батч 500 строк/1 сек) → Loki HTTP push.

## logcli

Grafana Explore не используем — вкладка зависает из-за тяжёлого UI. logcli гораздо
быстрее и работает как grep.

```bash
# Все TRACE строки за последние 5 минут
LOKI_ADDR=http://192.168.2.109:13100 logcli query '{source="tcp-bridge"} |~ "TRACE"' --since=5m

# По конкретному trace_id (tid)
LOKI_ADDR=http://192.168.2.109:13100 logcli query '{source="tcp-bridge"} |~ "TRACE" |~ "tid=42"' --since=10m

# Только для одного сервиса
LOKI_ADDR=http://192.168.2.109:13100 logcli query '{source="tcp-bridge"} |~ "TRACE" |~ "gateway"' --since=5m

# Только CAS callback
LOKI_ADDR=http://192.168.2.109:13100 logcli query '{source="tcp-bridge"} |~ "cas_cb"' --since=5m --limit=50
```

## Полный трейс блок-брейка (пример)

```
simcore publish_ack           148us    ← optimistic ACCEPTED клиенту
cas_cb   complete             590us    ← CAS в chunkd завершился успешно
simcore publish_block_changed  12us    ← broadcast другим игрокам
gateway skip_relay_to_source    0us    ← source_player_id совпал — не дублируем
client  recv_block_ack          0us    ← клиент получил ACCEPTED
```

## Типичные проблемы

1. **TRACE строк нет в Loki** — проверь что `GTNH_LOG_LEVEL=trace` в run.sh
2. **Grafana Explore не открывается** — используй logcli
3. **401 от Loki** — нужен `auth_enabled: false` в конфиге
4. **500 "at least 2 live replicas"** — нужен `replication_factor: 1` (single-instance)
5. **`trace_id=~"/.*/"` не работает** — `trace_id` это regex capture group,
   **НЕ** Loki-лейбл. Фильтровать только через `|~ "tid=42"` или `| regexp "(?P<trace_id>...)"`

## Дашборд Grafana

Дашборд лежит в `/mnt/nfs/model.json`. Три панели:

| Панель | Что показывает |
|--------|----------------|
| TRACE ops/s by service | Количество TRACE строк в секунду по сервисам |
| Avg latency by service + op | Среднее dur_us по (service, op) |
| TRACE log table | Сырые TRACE строки с разобранными полями (tid, svc, op, dur) |
