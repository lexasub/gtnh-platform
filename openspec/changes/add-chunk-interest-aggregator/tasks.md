# Tasks: Chunk Interest Aggregator

## Status: DEFERRED — не начинать реализацию до одобрения и до фикса бага рёбер

## 1. Протокол
- [ ] 1.1 Добавить в `protocol/*.fbs` сообщения: `ChunkInterestRegister`,
      `ChunkInterestUnregister` (client, ranges of item-id, chunk-scope), `ChunkBatchNotify`
      (deferred payload, ranged item-id found), `ChunkUnloadNotify` (chunk bounds).
- [ ] 1.2 Определить wire-константы GatewayMsg и добавить в роутер темы
      (`chunk.interest.*`, `chunk.batch.notify`).

## 2. Сервис `chunk_interest`
- [ ] 2.1 Каркас сервиса (C++), unit-карта: подписки + router client.
- [ ] 2.2 Владение статусом chанков: перенос загруженности из simcore.
- [ ] 2.3 Структура регистраций интересов (диапазоны item-id per client).
- [ ] 2.4 Обработчик `world.chunk.loaded.compressed`: распарсить палитру, отфильтровать по
      интересам, отложить в debounce-очередь.
- [ ] 2.5 Debounce/батч по времени: рассылка `ChunkBatchNotify` пачкой.
- [ ] 2.6 Обработчик `world.blocks.changed` (дублирование из simcore): точечные изменения.
- [ ] 2.7 Обработчик `chunk.unloaded`: рассылка `ChunkUnloadNotify` с границами.

## 3. simcore
- [ ] 3.1 Отдать владение статусом загруженности; слушать batch/unload от агрегатора.
- [ ] 3.2 Дублировать в агрегатор то, что шлёт в chunkd при клике/установке.
- [ ] 3.3 Сносить сущности/машины при `ChunkUnloadNotify`.

## 4. pipenetworkd
- [ ] 4.1 Подписаться на `ChunkBatchNotify`/`ChunkUnloadNotify`.
- [ ] 4.2 Строить рёбра для найденных труб (или подтверждать из батча).
- [ ] 4.3 Сносить рёбра у узлов вне выгруженного чанка.

## 5. Тесты
- [ ] 5.1 Unit: debounce-агрегация (K событий → 1 батч по таймеру).
- [ ] 5.2 Unit: фильтр по диапазонам — находит нужный id в палитре чанка.
- [ ] 5.3 Integration: новая труба в повторно загруженном чанке нотифицируется → рёбо в pipenetworkd.
- [ ] 5.4 Integration: unload → pipenetworkd сносит рёдра.
- [ ] 5.5 Perf smoke: 1000-чанк загрузка не спамит (количество рассылок ≤ батчам).
