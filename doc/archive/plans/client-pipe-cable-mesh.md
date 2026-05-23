# Plan: Client Pipe & Cable Mesh Rendering

**Based on**: `doc/EPICS/5-transport-pipes-cables/tasks/task-7.md` (pipe mesh), `task-19.md` (cable visuals)
**Status**: 🔵 Planning
**Last update**: 2026-06-28

## Current state

- `Render/` has skeleton headers: `PipeMeshBuilder.h`, `CableMeshBuilder.h`, `CableColors.h`, `FaceTextureRegistry.h`
- `Render/ChunkMeshBuilder.cpp` — только solid-блоки (stone/dirt/grass), нет pipe/cable-ветки
- Old stubs в корне `game_client/` — удалены

## Структура pipe/cable block IDs

```
item_pipe         = 62
dense_item_pipe   = 64
fluid_pipe        = 61
dense_fluid_pipe  = 65

cable_tin         = 80
cable_copper      = 81
cable_gold        = 82
cable_alu         = 83
cable_tungsten    = 84
cable_platinum    = 85
```

## Шаги реализации

### Шаг A: PipeMeshBuilder — реализация mesh генерации

**Файлы**: `Render/PipeMeshBuilder.h` → `Render/PipeMeshBuilder.cpp` (NEW)

**Задача**: generate pipe geometry как программатик mesh (не модель):
- Центральный цилиндр/туба через блок (занимает ~60% блока, не full cube)
- На каждой грани: если сосед — такой же тип трубы → открытое соединение; если нет → заглушка (end cap / flange)
- FaceMask: 6 бит (DOWN, UP, NORTH, SOUTH, WEST, EAST)
- Выход: `ChunkMeshBuilder::MeshData` (вектор `BlockVertex` + индексы) — чтобы можно было слить с чанковым мешем

**Детали**:
- `PipeMeshBuilder::buildPipeMesh(x, y, z, pipeType, faceMask)` → `MeshData`
- Использовать тот же `BlockVertex` формат (float xyz, uint8 normal[4], uint8 color[4], float uv)
- Цвет: серый металл для обычных труб, tint для dense
- UV: заглушка (0,0) — атлас текстур труб пока не нужен, можно flat color
- Для жидкости: доп. внутренний цилиндр (task-12, deferred)

### Шаг B: CableMeshBuilder — реализация mesh генерации

**Файлы**: `Render/CableMeshBuilder.h` → `Render/CableMeshBuilder.cpp` (NEW)

**Задача**: generate cable geometry как тонкая проволока через блок:
- Тонкая проволока ~25% объёма блока (не full cube, не как труба)
- Соединения с соседними кабелями → continuous wire
- Нет соединения → termination point
- Использовать `CableColors.h` для окраски

**Детали**:
- `CableMeshBuilder::buildCableMesh(x, y, z, tier, faceMask)` → `MeshData`
- Цвет из `CABLE_COLORS[tier]` (glm::vec3 → BlockVertex.color)
- Геометрия: thin cylinder вдоль каждого соединения + маленькая сфера/куб в центре

### Шаг C: Block render type detection

**Файлы**: NEW: `Render/BlockRenderRegistry.h/.cpp` или просто функции в `Render/PipeMeshBuilder.h`

**Задача**: определять, какой рендер-тип у блока по blockId:
- `isPipeBlock(blockId)` → true для 61, 62, 64, 65
- `isCableBlock(blockId)` → true для 80-85
- `getPipeType(blockId)` → PipeType enum
- `getCableTier(blockId)` → tier 1-4

Данные уже есть в `PipeMeshBuilder.h` (`pipeTypeToBlockId()`) и `CableTypes.h` (на pipe_network стороне).

### Шаг D: ChunkMeshBuilder — pipe/cable integration

**Файл**: `Render/ChunkMeshBuilder.cpp`

**Задача**: расширить `Build()` — если блок pipe/cable, то вызывать MeshBuilder вместо full-block.

**Логика**:
```
for each block in chunk:
    if block == 0 → continue
    if isPipeBlock(block):
        faceMask = detectConnections(...)
        mesh = PipeMeshBuilder::buildPipeMesh(x, y, z, pipeType, faceMask)
        merge mesh into chunk mesh (append vertices + indices)
    elif isCableBlock(block):
        faceMask = detectConnections(...)
        mesh = CableMeshBuilder::buildCableMesh(x, y, z, tier, faceMask)
        merge mesh into chunk mesh
    else:
        // existing solid-block logic
```

### Шаг E: FaceTextureRegistry — use case (deferred)

`FaceTextureRegistry.h` — константы для маркировки граней машин (INPUT/OUTPUT/ENERGY). Нужны будут когда:
- Машины начнут рендериться с разными текстурами граней
- Task B7 из 4-electric-tools-wrench

Пока не трогаем.

## Порядок выполнения

```
A (PipeMeshBuilder .cpp) ──→ D (ChunkMeshBuilder integration)
                                  ↑
B (CableMeshBuilder .cpp) ────┘
                                  ↓
C (BlockRenderRegistry) ───────┘
```

A и B независимы — можно параллельно.

## Критерии готовности

- [ ] Pipe блоки рендерятся как трубы (не full cube)
- [ ] Cable блоки рендерятся как тонкая проволока
- [ ] Соединения между соседними трубами/кабелями работают
- [ ] FaceMask детектится через ChunkNeighborCache (соседи в том же чанке или соседнем)
- [ ] Pipe/cable блоки корректно показываются в GameClient (при заходе/постановке)
- [ ] Перформанс: pipe/cable meshing не увеличивает время сборки чанка > 10%
- [ ] Raycast работает через pipe/cable геометрию (кликабельность)

## Отложено

- ParticleSystem для эффектов перегрева/взрыва (task-19.4)
- Wireframe/texture для атласа труб/кабелей (сейчас flat color)
- Fluid level animation в fluid pipes (task-12)
- Зависимые эпики: CableGraph (сервер), overheat/explosion, loss
