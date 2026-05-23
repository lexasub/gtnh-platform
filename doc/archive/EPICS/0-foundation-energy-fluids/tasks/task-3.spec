{
  "title": "EntityStateStore — разрывы между документацией и кодом",
  "affected_services": ["EntityStateStore (NEW)", "MetaDB (Go)", "Protocol", "SimulationCore"],
  "description": "Анализ расхождений между существующей документацией (docs/ EPICS) и фактическим состоянием кодовой базы. Документирует найденные проблемы и план их устранения.",

  "ecs_components": [],

  "flatbuffers_schemas": [
    {
      "name": "Проблема 1: Дублирование InventorySlot / InventoryUpdate в entitystatestore.fbs",
      "fields": [
        {"issue": "entitystatestore.fbs определяет собственные InventorySlot (строка 14) и InventoryUpdate (строка 23), хотя core.fbs уже определяет InventorySlot (строка 106) и InventoryUpdate (строка 115) в том же namespace Protocol"},
        {"impact": "Ошибка компиляции flatc: duplicate table name 'InventorySlot' in namespace 'Protocol'"},
      {"fix": "✅ DONE: дубликаты удалены, entitystatestore.fbs → meta_db.fbs, используется include 'core.fbs'"}
    ]
    },
    {
      "name": "Проблема 2: EntityStateStore описан как C++/LMDB, но код говорит о Go/SQLite",
      "fields": [
        {"doc_claim": "item-inventory task-6: 'EntityStateStore is a C++ service responsible for persisting generic TileEntity state data... LMDB is selected'"},
        {"doc_claim": "machines-multiblocks task-5: 'EntityStateStore Service... LMDB (Lightning Memory-Mapped Database)'"},
        {"actual_code": "entitystatestore.fbs содержал только player inventory RPC, которые относятся к MetaDB (Go/SQLite)"},
        {"fix": "✅ DONE: entitystatestore.fbs → meta_db.fbs. Создан tile_entity_store.fbs с GetTileState/SetTileState."}
      ]
    }
  ],

  "inputs": [],
  "outputs": [],

  "constraints": [
    "✅ Fixed: meta_db.fbs отвечает за player inventory, tile_entity_store.fbs за TileEntity state",
    "TileEntity state и Player inventory — разные сервисы с разными бэкендами"
  ],

  "tests": []
}
