# SPEC-gtnh-libs

# GTNH Static Libraries Specification

## Overview

Набор мелких статических библиотек вместо одного монолитного `gtnh_core`. Каждая библиотека делает одну вещь, имеет свои тесты, линкуется только куда нужно. Никакого "жирного core".

## Принципы

1. **Одна библиотека = одна ответственность**. MachineRegistry не тянет FastNoise.
2. **Линкуй только то, что используешь**. chunkd не тянет ECS components.
3. **Каждая lib имеет свои Catch2 тесты**.
4. **Нет циклических зависимостей**. Граф зависимостей — DAG.
5. **Старый код остаётся на месте**, пока все потребители не переедут.

## Состав библиотек

```
src/libs/
├── gtnh_types/          — Vec3i, AABB, ChunkPos, Face, блоковые константы, enum helpers
├── gtnh_protocol/       — FlatBuffers schema validation, message type checks
├── gtnh_registry/       — MachineRegistry, ItemRegistry, FluidRegistry, RecipeRegistry
├── gtnh_ecs_core/       — EnergyStorage, Position, InventoryContainer (стабильные компоненты)
├── gtnh_pipegraph/      — PipeNetworkGraph (BFS), CableGraph (packets), EnergyPacket
├── gtnh_chunk/          — Chunk format (blocks[32³] uint16_t, serialization)
├── gtnh_worldgen/       — FastNoise wrapper, OreConfig, OreGenerator, heightmap algorithms
└── gtnh_test/           — Catch2 test fixtures, mock services (test-only, не линкуется в релиз)
```

---

## 1. gtnh_types

**Заголовочная библиотека (header-only или почти)**. Никаких зависимостей, кроме стандартной библиотеки.

```
src/libs/gtnh_types/include/gtnh/types/
├── Vec3i.h            — {int32_t x, y, z} с операторами +, -, ==, hash
├── AABB.h             — Axis-Aligned Bounding Box (min/max Vec3i)
├── ChunkPos.h         — chunkX, chunkZ, toWorldPos(), fromWorldPos()
├── Face.h             — enum Face { DOWN, UP, NORTH, SOUTH, WEST, EAST }, faceOffset[]
├── BlockConstants.h   — BLOCK_AIR=0, BLOCK_STONE=1, BLOCK_GRASS=... и ore IDs
├── SideRole.h         — enum SideRole { INPUT, OUTPUT, ENERGY, FLUID_IN, FLUID_OUT, ANY, NONE }
├── ItemSlot.h         — {uint16_t item_id, uint8_t count, uint16_t meta}
├── EnergyType.h       — enum EnergyType { ELECTRICITY, HEAT, STEAM }, tierVoltage()
└── EnergyPacket.h     — {uint32_t voltage, uint8_t ampCount, uint64_t sourceId, uint64_t targetId}
```

**CMakeLists.txt:**
```cmake
add_library(gtnh_types INTERFACE)
target_include_directories(gtnh_types INTERFACE include)
```

**Кто линкует:** все. Зависимостей нет.

---

## 2. gtnh_protocol

**FlatBuffers schema validation.** Проверяет .fbs файлы при сборке, а не в рантайме.

```
src/libs/gtnh_protocol/
├── include/gtnh/protocol/
│   └── SchemaValidator.h    — ValidateAll(), ValidateFile(), UnionTypeCheck()
├── src/
│   └── SchemaValidator.cpp
└── tests/
    └── SchemaValidationTest.cpp  — 4 теста
```

**Зависимости:** `flatbuffers::flatbuffers`

**CMakeLists.txt:**
```cmake
add_library(gtnh_protocol STATIC src/SchemaValidator.cpp)
target_include_directories(gtnh_protocol PUBLIC include)
target_link_libraries(gtnh_protocol PUBLIC flatbuffers::flatbuffers)
```

**Кто линкует:** CI, pre-commit hook. Не линкуется в сервисы (только тесты + валидация схем).

---

## 3. gtnh_registry

**Реестры машин, предметов, жидкостей, рецептов.** Data-driven из CSV/JSON.

```
src/libs/gtnh_registry/
├── include/gtnh/registry/
│   ├── MachineRegistry.h    — Load(), Get(), IsMachine(), IsConsumer(), IsProducer()
│   ├── ItemRegistry.h       — GetItem(), IsItem(), itemId(name)
│   ├── FluidRegistry.h      — GetFluid(), IsFluid(), fluid properties
│   └── RecipeRegistry.h     — FindRecipe(), machine recipes
├── src/
│   ├── MachineRegistry.cpp
│   ├── ItemRegistry.cpp
│   ├── FluidRegistry.cpp
│   └── RecipeRegistry.cpp
└── tests/
    └── RegistryTest.cpp   — 8 тестов (MachineRegistry + ItemRegistry + FluidRegistry)
```

**Зависимости:** `gtnh_types` (ItemSlot, EnergyType), `fmt::fmt`, `nlohmann_json::nlohmann_json`

**Кто линкует:** `simulation_core`, `pipe_network`, `game_client`, `chunk_store`

**CMakeLists.txt:**
```cmake
add_library(gtnh_registry STATIC
    src/MachineRegistry.cpp src/ItemRegistry.cpp
    src/FluidRegistry.cpp src/RecipeRegistry.cpp)
target_link_libraries(gtnh_registry PUBLIC gtnh_types fmt::fmt nlohmann_json::nlohmann_json)
```

---

## 4. gtnh_ecs_core

**Стабильные ECS компоненты.** Только pure data containers без orchestration логики.

```
src/libs/gtnh_ecs_core/include/gtnh/ecs/
├── EnergyStorage.h        — {capacity, current, maxInput, maxOutput, tier, type} + helpers
├── Position.h             — {x, y, z} (может дублировать Vec3i — для единообразия ECS)
└── InventoryContainer.h   — {entity_type, slot_count, slots} + getSlot/setSlot/addItem/removeItem
```

**Header-only.** Зависимости: `gtnh_types` (EnergyType, ItemSlot).

**Кто линкует:** `simulation_core`

---

## 5. gtnh_pipegraph

**Алгоритмы графов для PipeNetwork.** BFS, распределение энергии/теплоты (continuous), packet-based CableGraph.

```
src/libs/gtnh_pipegraph/
├── include/gtnh/pipegraph/
│   ├── PipeNetworkGraph.h     — discoverNetwork(), rebuildNetworks(), distributeEnergy() (HEAT)
│   └── CableGraph.h           — injectPacket(), collectPackets(), tick(), overheat check
├── src/
│   ├── PipeNetworkGraph.cpp
│   └── CableGraph.cpp
└── tests/
    └── PipeGraphTest.cpp   — 6 тестов (BFS connected/disconnected, energy dist, packet inj/overheat)
```

**Зависимости:** `gtnh_types` (EnergyPacket, EnergyType), `fmt::fmt`

**Кто линкует:** `pipe_network`, `simulation_core`

**Важно:** `CableGraph` и `PipeNetworkGraph` — в одной библиотеке, т.к. разделяют базовые структуры (PipeNode, PipeEdge). Если в будущем станет слишком жирно — разделим.

---

## 6. gtnh_chunk

**Формат чанка и сериализация.** Без LMDB.

```
src/libs/gtnh_chunk/
├── include/gtnh/chunk/
│   └── Chunk.h               — blocks[32][32][32] uint16_t, meta, multiblock + serialize/deserialize
├── src/
│   └── Chunk.cpp
└── tests/
    └── ChunkTest.cpp       — 4 теста (create, set/get block, roundtrip serialization)
```

**Зависимости:** `gtnh_types` (Vec3i), `flatbuffers::flatbuffers`

**Кто линкует:** `chunk_store`, `game_client`

---

## 7. gtnh_worldgen

**Примитивы генерации мира.** FastNoise wrapper, конфиг руд, генератор жил.

```
src/libs/gtnh_worldgen/
├── include/gtnh/worldgen/
│   ├── OreConfig.h         — load ores.json, getOre(), allOres()
│   ├── OreGenerator.h      — generateOres(chunkX, chunkZ, heightMap, blocks)
│   └── TerrainNoise.h      — FastNoise wrapper: heightNoise, caveNoise, oreNoise
├── src/
│   ├── OreConfig.cpp
│   ├── OreGenerator.cpp
│   └── TerrainNoise.cpp
└── tests/
    └── WorldGenTest.cpp    — 4 теста (OreConfig parse, OreGenerator vein shape, noise consistency)
```

**Зависимости:** `gtnh_types` (BlockConstants), `gtnh_chunk` (Chunk format), `FastNoise::FastNoise`, `nlohmann_json`

**Кто линкует:** `chunk_store` (который включает WorldGenerator)

---

## 8. gtnh_test (test-only)

**Фикстуры и моки для тестирования сервисов.** Не линкуется в релиз.

```
src/libs/gtnh_test/include/gtnh/test/
├── MockRouterClient.h     — MessageRouter мок для тестов
├── MockChunkStore.h       — ChunkStore мок
├── TestRegistry.h         — RegistryBuilder: создать MachineRegistry из inline данных
└── ECSFixture.h           — entt::registry + component factory для тестов
```

**Зависимости:** `Catch2::Catch2`, `gtnh_types`, `gtnh_registry`, `gtnh_ecs_core`

**Линкуется:** только test executables, `target_link_libraries(... PRIVATE gtnh_test)`

---

## Граф зависимостей

```
gtnh_types (header-only, 0 dependencies)
    ├── gtnh_protocol       (flatbuffers)
    ├── gtnh_registry       (fmt, nlohmann_json)
    ├── gtnh_ecs_core       (header-only)
    ├── gtnh_pipegraph      (fmt)
    ├── gtnh_chunk          (flatbuffers)
    └── gtnh_worldgen       (FastNoise, nlohmann_json, gtnh_chunk)
            │
    ┌───────┴──────────────────┐
    │                          │
simulation_core            chunk_store
(линкует: gtnh_types,       (линкует: gtnh_types,
 gtnh_registry,              gtnh_chunk,
 gtnh_ecs_core,              gtnh_worldgen)
 gtnh_pipegraph)
    │
pipe_network
(линкует: gtnh_types,
 gtnh_pipegraph,
 gtnh_registry)
```

**Следствие:** можно сделать `make gtnh_registry` и собрать только его, без FastNoise, без PipeNetwork, без Chunk. Идеально для быстрой итерации.

---

## Структура директорий

```
src/
├── libs/
│   ├── gtnh_types/
│   │   ├── CMakeLists.txt
│   │   ├── include/gtnh/types/
│   │   └── tests/CMakeLists.txt + tests
│   ├── gtnh_protocol/
│   │   ├── CMakeLists.txt
│   │   ├── include/gtnh/protocol/
│   │   ├── src/
│   │   └── tests/
│   ├── gtnh_registry/   (аналогично)
│   ├── gtnh_ecs_core/
│   ├── gtnh_pipegraph/
│   ├── gtnh_chunk/
│   ├── gtnh_worldgen/
│   └── gtnh_test/
├── services/          (остаются как есть, линкуют libs)
├── protocol/          (остаётся как есть)
└── data/              (остаётся как есть)
```

---

## План миграции

### Phase 0: gtnh_types (30 мин)
1. Создать `src/libs/gtnh_types/`
2. Перенести Vec3i, Face, BlockConstants, SideRole, ItemSlot, EnergyType, EnergyPacket
3. Все — header-only, никаких .cpp
4. Обновить include пути во всех файлах, которые их используют

### Phase 1: gtnh_registry (2 часа)
1. Скопировать MachineRegistry из `src/libs/machine_registry/` в `src/libs/gtnh_registry/`
2. Создать ItemRegistry (обёртка над items.csv)
3. Создать FluidRegistry
4. Написать `RegistryTest.cpp` (8 тестов)
5. Переключить `simulation_core` на `gtnh_registry`
6. Заменить `isMachineBlock()` на `MachineRegistry::GetMachineInfo()`

### Phase 2: gtnh_ecs_core (1 час)
1. Скопировать EnergyStorage.h, Position.h, InventoryContainer.h
2. Сделать namespace gtnh::ecs
3. Написать component serialization tests
4. Переключить simulation_core на `gtnh::ecs::EnergyStorage`

### Phase 3: gtnh_pipegraph (2 дня)
1. Извлечь PipeNetworkGraph из `src/services/pipe_network/PipeNetwork.h/.cpp`
2. Извлечь CableGraph (packet-based)
3. Оставить MessageRouterClient и PipeNetworkService в сервисе
4. Написать `PipeGraphTest.cpp` (6 тестов)

### Phase 4: gtnh_chunk (1 день)
1. Извлечь Chunk структуру из src/services/chunk_store/Chunk/
2. Написать serialize/deserialize + FlatBuffer конвертацию
3. Написать `ChunkTest.cpp` (4 теста)

### Phase 5: gtnh_worldgen (1 день)
1. Создать OreConfig (JSON parser)
2. Создать OreGenerator (3D синусоидальный шум жил)
3. Создать TerrainNoise (FastNoise wrapper)
4. Написать `WorldGenTest.cpp` (4 теста)
5. Заменить хардкод `block_id=5` в WorldGenerator.cpp

### Phase 6: gtnh_protocol (1 день)
1. SchemaValidator для всех .fbs схем
2. Добавить в CMake как pre-build step
3. Написать `SchemaValidationTest.cpp`

### Phase 7: gtnh_test (2 часа)
1. MockRouterClient, MockChunkStore
2. TestRegistry (inline registry для тестов без CSV)
3. ECSFixture (entt::registry + common components)

---

## CI команды

```bash
# Собрать только registry
cmake --build build --target gtnh_registry

# Собрать и прогнать тесты только для pipegraph
cmake --build build --target gtnh_pipegraph_test && ./build/libs/gtnh_pipegraph/gtnh_pipegraph_test

# Валидация схем FlatBuffers
cmake --build build --target gtnh_protocol_test && ./build/libs/gtnh_protocol/gtnh_protocol_test

# Все тесты либ
ctest --test-dir build -R "gtnh_"

# Тесты конкретного сервиса + его lib зависимостей
cmake --build build --target simulation_core_test && ./build/services/simulation_core/simulation_core_test
```

---

## Что НЕ входит в libs (остаётся в services)

| Компонент | Почему не в libs |
|-----------|-----------------|
| MachineSystem, GeneratorSystem, BoilerSystem | Игровая логика, постоянно меняется |
| ActionDispatcher | Маршрутизация действий, зависит от протокола |
| MultiblockController, Pattern detection | Multiblock специфика SimulationCore |
| HeatTransferSystem | Игровая механика (теплопроводность) |
| PipeNetworkService | Сервисный слой (MessageRouter, топики) |
| Gateway, ChunkStore ServerWorld | Инфраструктурные сервисы |
| ImGui / MachineWindow | Клиентский код |
| RecipeManager RPC сервис | Сетевой слой рецептов |
| Все ECS системы (MachineSystem и т.д.) | Оркестрация, не stable |
| MessageRouterClient | Сетевая специфика |
| EntityStateStore (сервис) | Инфраструктура |
