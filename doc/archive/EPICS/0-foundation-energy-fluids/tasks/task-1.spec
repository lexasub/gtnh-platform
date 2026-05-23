{
  "title": "EntityStateStore — архитектура сервиса и протокол",
  "affected_services": ["EntityStateStore (NEW)", "SimulationCore (consumer)", "MessageRouter (transport)"],
  "description": "EntityStateStore — сервис персистентного состояния машин (TileEntity). Хранит energy storage, fluid tanks, machine inventories, workbench state. Отдельный dumb storage сервис, ничего не знающий о логике машин. Отвечает за запись/чтение бинарных блобов состояния по координатам (dim, x, y, z).",

  "ecs_components": [
    "MachineState { energy_storage: EnergyStorage, fluid_tanks: [FluidTank], inventory: MachineInventory }"
  ],

  "flatbuffers_schemas": [
    {
      "name": "EntityStateStoreFrame",
      "fields": [
        {"name": "payload", "type": "EntityStateStorePayload (required)", "description": "Union: EntityStateStoreMessage | EntityStateStoreReply"}
      ]
    },
    {
      "name": "EntityStateStoreMessage",
      "fields": [
        {"name": "req_id", "type": "uint32"},
        {"name": "request", "type": "EntityStateStoreRequest (union)"}
      ]
    },
    {
      "name": "EntityStateStoreReply",
      "fields": [
        {"name": "req_id", "type": "uint32"},
        {"name": "response", "type": "EntityStateStoreResponse (union)"}
      ]
    },
    {
      "name": "EntityStateStoreRequest (union)",
      "fields": [
        {"name": "GetInventoryReq", "type": "GetInventoryReq", "description": "⛔ должно быть в MetaDB, не здесь"},
        {"name": "SetInventorySlotReq", "type": "SetInventorySlotReq", "description": "⛔ должно быть в MetaDB, не здесь"}
      ]
    }
  ],

  "inputs": [
    {"name": "SetStateReq", "source": "SimulationCore → EntityStateStore", "description": "SimulationCore сохраняет состояние машины по координатам"},
    {"name": "GetStateReq", "source": "SimulationCore → EntityStateStore", "description": "SimulationCore запрашивает состояние машины по координатам"}
  ],

  "outputs": [
    {"name": "SetStateResp", "target": "EntityStateStore → SimulationCore", "description": "Подтверждение сохранения"},
    {"name": "GetStateResp", "target": "EntityStateStore → SimulationCore", "description": "Блоб состояния машины (или пустой)"}
  ],

  "constraints": [
    "EntityStateStore не знает о содержимом блоба — dumb storage",
    "Ключ: dim|x|y|z, значение: бинарный blob",
    "Один писатель (SimulationCore), много читателей"
  ],

  "tests": [
    "Test: SetState + GetState — запись и чтение блоба по координатам",
    "Test: GetState для несуществующих координат — пустой результат",
    "Test: SetState для разных dim — независимость измерений"
  ]
}
