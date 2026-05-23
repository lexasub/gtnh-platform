{
  "title": "EntityStateStore — план имплементации (MVP)",
  "affected_services": ["EntityStateStore (NEW)", "MetaDB (Go)", "Protocol", "MessageRouter"],
  "description": "Пошаговый план реализации EntityStateStore для MVP: от фикса схемы до работающего прототипа через MessageRouter. Учитывает найденные проблемы с дублированием типов и несоответствием документации.",

  "ecs_components": [],

  "flatbuffers_schemas": [
    {
      "name": "Шаг 1: Фикс entitystatestore.fbs → rename to meta_db.fbs",
      "fields": [
        {"action": "✅ DONE: дублирующиеся таблицы удалены"},
        {"action": "✅ DONE: файл переименован в meta_db.fbs"},
        {"action": "✅ DONE: root_type обновлён на MetaDBFrame"},
        {"action": "InventorySlot использует include 'core.fbs'"}
      ]
    },
    {
      "name": "Шаг 2: Создать tile_entity_store.fbs",
      "fields": [
        {"action": "Новый файл src/protocol/tile_entity_store.fbs"},
        {"action": "RPC: GetTileState(dim, x, y, z) → blob"},
        {"action": "RPC: SetTileState(dim, x, y, z, blob) → ack"},
        {"action": "root_type: TileEntityStoreFrame"},
        {"action": "Без InventorySlot — это shared тип из core.fbs"}
      ]
    },
    {
      "name": "Шаг 3: C++ сервис EntityStateStore",
      "fields": [
        {"action": "src/services/entity_state_store/ — новый сервис"},
        {"action": "Бэкенд: LMDB (liblmdb)"},
        {"action": "Ключ: packed uint64_t из dim|x|y|z"},
        {"action": "Значение: сырой blob, без интерпретации"},
        {"action": "RPC через MessageRouter: TileEntityStoreFrame"}
      ]
    }
  ],

  "inputs": [
    {"name": "RPC calls", "source": "SimulationCore → EntityStateStore", "description": "SimulationCore вызывает GetTileState/SetTileState во время tick/chunk load/unload"}
  ],

  "outputs": [
    {"name": "State blob", "target": "EntityStateStore → SimulationCore", "description": "Блоб состояния TileEntity по запросу"}
  ],

  "constraints": [
    "EntityStateStore — dumb storage, не знает о содержимом",
    "Hot path: SimulationCore читает/пишет состояние каждый tick",
    "Player inventory остаётся в MetaDB (Go/SQLite) — не путать"
  ],

  "tests": [
    "Test: GetTileState для несуществующих координат → пустой blob",
    "Test: SetTileState + GetTileState → данные сохраняются и читаются",
    "Test: Разные dim с одинаковыми x,y,z — независимые записи",
    "Test: LMDB не блокируется при конкурентных чтениях"
  ]
}
