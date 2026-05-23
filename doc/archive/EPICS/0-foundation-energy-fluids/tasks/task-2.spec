{
  "title": "EntityStateStore — бэкенд хранения и формат блоба",
  "affected_services": ["EntityStateStore (NEW)", "Protocol", "SimulationCore (consumer)"],
  "description": "Decision record: выбор бэкенда (SQLite vs LMDB), формат сериализации блоба состояния машины, и схема данных для energy storage и fluid tanks внутри MachineState.",

  "ecs_components": [
    "MachineState { energy: EnergyStorage, fluid: [FluidTank], inventory: MachineInventory }",
    "EnergyStorage { capacity: int32, current: int32, maxInput: int32, maxOutput: int32, tier: int32 }",
    "FluidTank { fluid_id: uint32, amount: int32, capacity: int32 }",
    "MachineInventory { slots: [ItemStack], size: uint8 }"
  ],

  "flatbuffers_schemas": [
    {
      "name": "MachineState (внутренний формат блоба — НЕ в протоколе, а внутри EntityStateStore)",
      "fields": [
        {"name": "energy", "type": "EnergyStorage (struct)", "description": "Текущее состояние energy storage машины"},
        {"name": "fluids", "type": "[FluidTank]", "description": "Массив жидкостных танков машины (0-N)"},
        {"name": "inventory", "type": "MachineInventory", "description": "Внутренний инвентарь машины (вход/выход/топливо)"},
        {"name": "nbt_tags", "type": "[NBTTag] (опционально)", "description": "Расширяемые метаданные для GTNH-специфичных полей"}
      ]
    },
    {
      "name": "EnergyStorage",
      "fields": [
        {"name": "capacity", "type": "int32"},
        {"name": "current_energy", "type": "int32"},
        {"name": "max_input", "type": "int32"},
        {"name": "max_output", "type": "int32"},
        {"name": "tier", "type": "int32"}
      ]
    },
    {
      "name": "FluidTank",
      "fields": [
        {"name": "fluid_id", "type": "uint32"},
        {"name": "amount", "type": "int32"},
        {"name": "capacity", "type": "int32"}
      ]
    },
    {
      "name": "MachineInventory",
      "fields": [
        {"name": "size", "type": "uint8"},
        {"name": "slots", "type": "[ItemStack]"}
      ]
    }
  ],

  "service_architecture": "Блоб машины сериализуется SimulationCore через FlatBuffers (schema MachineState) и отправляется в EntityStateStore как opaque blob [uint8]. EntityStateStore НЕ парсит содержимое блоба — это задача SimulationCore. Блоб хранится как есть. При загрузке чанка Gateway запрашивает блоб по координатам, SimulationCore десериализует и восстанавливает ECS-компоненты машины.",

  "inputs": {
    "SimulationCore → EntityStateStore": {
      "inputs": ["dim (uint32)", "x (uint32)", "y (uint32)", "z (uint32)", "blob ([uint8])"],
      "outputs": ["success (bool)"]
    },
    "EntityStateStore → SimulationCore": {
      "inputs": ["dim (uint32)", "x (uint32)", "y (uint32)", "z (uint32)"],
      "outputs": ["blob ([uint8])"]
    }
  },

  "constraints": [
    "Бэкенд: SQLite через существующий MetaDB (Go) — не создавать отдельный LMDB сервис на MVP",
    "RADIО: MetaDB уже работает на SQLite, имеет go-sqlite3 зависимость и TCP обработчик. EntityStateStore — новая таблица в той же БД или отдельный .sqlite файл",
    "LMDB отложен на L2: когда машины будут обновляться каждый тик (>1000 rps), LMDB даст выигрыш в zero-copy mmap чтении",
    "Ключ: строка формата 'entities/{dim}/{x}/{y}/{z}' — читаемый, совместимый с SQLite TEXT PRIMARY KEY",
    "Блоб: сырой FlatBuffer MachineState (не JSON, не другой формат) — SimulationCode сериализует через flatbuffers::FlatBufferBuilder, EntityStateStore сохраняет как BLOB",
    "Размер блоба: ограничен 64KB (лимит MessageRouter payload). Для машин с большими инвентарями — пост-MVP чанкование",
    "NBT tags: опциональная таблица [key:string, value:[uint8]] внутри MachineState. Типизированные значения: int32, float, string, [uint8]. Позволяет GTNH машинам хранить кастомные поля без изменения схемы",
    "stack_limit в ItemStack: uint16 (1-65535), а не uint8. GTNH имеет стеки >64 (трубы до 128, клетки до 64, но некоторые предметы до 2^31). uint16 покрывает все GTNH кейсы (макс. стек в GTNH — 2^31, но на MVP uint16 достаточно, uint32 — post-MVP)",
    "У ItemStack поле count — uint32 на схеме, но на MVP фактическое ограничение uint16. Поле meta — uint16 (как в core.fbs)"
  ],

  "test_requirements": [
    "Сериализация/десериализация MachineState через FlatBuffers: запись → чтение → exact match",
    "EnergyStorage: граничные значения (capacity=0, current > capacity, отрицательные значения)",
    "FluidTank: fluid_id=0 (пустой танк), amount=0, amount>capacity",
    "Блоб 0 байт: EntityStateStore сохраняет и возвращает",
    "Блоб 64KB (макс размер): сохраняется и возвращается без ошибок",
    "NBT: пустой массив, один тег, 100 тегов",
    "Перезапуск MetaDB: данные не теряются",
    "Ключ с максимальными координатами (dim=255, x/y/z=2^32-1): без коллизий"
  ]
}
