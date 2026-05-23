# EPIC: Multiblocks — Full Gameplay (L2)

**Layer**: L2  
**Статус**: 🔴 Не начато  
**Base**: `1-gameplay-machines-multiblocks` (L2 deferred)  
**Зависимости**: SpatialIndex, MachineSystem (L1), EntityStateStore, RecipeManager

## Userflow диаграммы

- `doc/userflow/09-multiblocks.puml` — MB1: EBF, MB2: Large Boiler, MB3: LCR
- `doc/userflow/04-machine-operations.puml` — M3: Multiblock formation

## Обзор

Полноценная gameplay логика для мультиблоков: обнаружение паттерна, controller management, специализированные tick системы для EBF, Large Boiler, Large Chemical Reactor, persistence, dissociation.

---

## Что уже есть (prototype) — по реальному коду

### ✅ MultiblockController struct

**Файл**: `src/services/simulation_core/ECS/components/MultiblockController.h`
```cpp
struct MultiblockController {
    uint64_t id = 0;       // controller_id
    uint32_t x = 0, y = 0, z = 0;  // anchor position
    std::vector<uint32_t> blocks;   // packed positions (xyz)
};
```
Используется в SimulationEngine для хранения controller registry.

### ✅ matchElectrolyser()

**Файл**: `src/services/simulation_core/ECS/SimulationEngine.cpp` (lines 141-183)

Хардкоженный 3x3x3 паттерн:
```cpp
const std::vector<std::tuple<int32_t, int32_t, int32_t>> ELECTROLYSER_PATTERN = {
    {-1,-1,-1}, {-1, 0,-1}, {-1, 1,-1},
    { 0,-1,-1}, { 0, 0,-1}, { 0, 1,-1},
    {-1,-1, 0}, {-1, 0, 0}, {-1, 1, 0},
    // ... 18 offsets total
};
```

### ✅ registerController()

В SimulationEngine — добавляет MultiblockController в registry, вызывает `onMachineCreated`.

### ✅ BlockEntityUpdate (частично)

MachineSystem публикует `publishBlockEntityUpdate` для managed_externally машин.
Нет специфических multiblock полей (structure_valid, hatches, covers).

### ✅ MachineSystem 3-pass tick

С managed_externally флагом — готов для multiblock→reciped маршрутизации.

### ✅ EntityStateStore

LMDB сервис на TCP :5200. Работает для WorkbenchStateManager.
**Нет** специфической multiblock persistence.

### ✅ world.block_entity.update → reciped flow

Callback `onMachineCreated` → `world.block_entity.update` → reciped сервис.

## ❌ Что НЕ сделано (по факту из кода)

| Задача | Статус | Детали |
|--------|--------|--------|
| **SpatialIndex** | ❌ **Только stub** | `src/services/spatial_index/main.cpp` — `int main() { return 0; }`. Ни R-tree, ни Octree. Критическая зависимость. |
| **Generic pattern library** | ❌ | Только `ELECTROLYSER_PATTERN` (хардкоженный 3x3x3). Нет системы для EBF (3x3x4), Boiler, LCR. |
| **MultiblockSystem** | ❌ | Нет отдельной ECS системы. Логика в SimulationEngine. Нужна выделенная система для L2. |
| **Topics: sim.multiblock.* ** | ❌ | Нет ни `sim.multiblock.created`, ни `sim.multiblock.destroyed` в MessageRouter. |
| **Dissociation** | ❌ | Нет обработки поломки anchor блока. `onBlockChanged(pos, AIR)` — не проверяет mb_id. |
| **Hatch detection** | ❌ | Нет системы поиска input/output/energy/fluid hatches для multiblock. |
| **EBF tick** | ❌ | Нет Heating coil tiers, heat requirement, muffler hatch. |
| **Large Boiler tick** | ❌ | Нет firebox, water→steam conversion, multi-size. |
| **LCR tick** | ❌ | Нет fluid+solid recipes, byproducts. |
| **Multiblock persistence** | ❌ | EntityStateStore не сохраняет MultiblockController. |
| **Client visuals** | ❌ | Нет multiblock highlight, bounding box, special rendering. |

---

## Раздел A: SpatialIndex Integration

### Что нужно сделать

SpatialIndex (R-tree/Octree) для быстрого поиска мультиблок-паттернов при установке блоков.

### Текущее состояние: только stub

`src/services/spatial_index/main.cpp`:
```cpp
int main() { return 0; }  // <-- ВСЁ. Сервис не реализован.
```

SpatialIndex отсутствует полностью — ни R-tree, ни Octree, ни query API.
Для L2 multiblocks это **критическая зависимость**: onBlockChanged не может эффективно искать блоки вокруг позиции без пространственного индекса.

### Что нужно сделать

**Альтернатива 1: Реализовать SpatialIndex (полноценно)**
- R-tree через Boost.Geometry (`bgi::rtree<AABB>`)
- 3 запроса: findBlocksInRadius(), isPatternComplete(), findAdjacent()
- Сервис на MessageRouter с топиками: `spatial.query.radius`, `spatial.query.pattern`

**Альтернатива 2: Без SpatialIndex (defer)**
- Использовать ChunkStore.getBlock() для каждого offset в паттерне
- O(blocks_in_pattern) — для 3x3x4 EBF = 36 запросов
- Работает для L2 MVP, но O(n) вместо O(log n)
- SpatialIndex отложить до L3

### Pattern library (generic)

```cpp
struct MultiblockPattern {
    uint32_t id;               // EBF = 1, BOILER = 2, LCR = 3
    std::string name;
    uint8_t size_x, size_y, size_z;  // 3, 3, 4 для EBF
    std::vector<PatternLayer> layers; // Каждый слой: [block_id][][]
    std::vector<HatchDef> hatches;    // Позиции для input/output/energy
    std::vector<BlockPos> controller_pos; // Где контроллер
};
```

**Detection flow (без SpatialIndex):**
1. `onBlockChanged(pos)` — триггер
2. Для каждого registered pattern:
   - ChunkStore.GetBlock(pos + offset) для всех offset в pattern
   - Сравнить block_id с ожидаемыми
   - Если все совпали → MultiblockController

### Критерии готовности

- [ ] SpatialIndex stub заменён на реализацию (или принято решение defer)
- [ ] Pattern registry в SimulationCore (минимум: EBF, Large Boiler, LCR)
- [ ] matchPattern(): generic функция, не хардкод как matchElectrolyser
- [ ] onBlockChanged → pattern check → MultiblockController entity
- [ ] Генерация machine_instance_id для multiblock

---

## Раздел B: EBF (Electric Blast Furnace)

### Архитектура

**Pattern:** 3×3×4
```
Layer 0: [C C C]  — касинг
         [C C C]
         [C C C]
Layer 1: [C A C]  — A = air
         [A H A]  — H = heating coil
         [C A C]
Layer 2: [C A C]
         [A H A]
         [C A C]
Layer 3: [C C C]
         [C M C]  — M = controller
         [C C C]
```

**Heating coils (определяют max heat):**
- Kanhal: 1800K
- Nichrome: 2700K
- TungstenSteel: 4500K

**Tick:**
1. Check coil ID → determine max_heat
2. Check recipe heat requirement
3. If max_heat >= required → process
4. If max_heat < required → pause
5. Consume EU/tick
6. Output через hatches

### Критерии готовности

- [ ] EBF pattern in pattern library
- [ ] EBFSystem или multiblock tick handler
- [ ] Heating coil ID → max_heat mapping
- [ ] Recipe heat requirement check
- [ ] Heat >= required → recipe progress
- [ ] Input/Output/Energy hatch detection
- [ ] Muffler hatch (top)

---

## Раздел C: Large Steam Boiler

### Архитектура

**Pattern:** 3×3×4 (или настраиваемый размер LP→HP)

**Компоненты:**
- Controller + casing
- Firebox (для горения топлива)
- Water intake hatches
- Steam output hatches
- Fuel input (solid: coal/charcoal, или fluid)

**Tick:**
1. Check fuel in firebox inventory
2. If fuel → burn, generate heat
3. If water in input hatch → convert water→steam
4. Publish steam to PipeNetwork fluid graph
5. If no water → heat buildup → overheat
6. If no fuel → cooldown → no steam

### Критерии готовности

- [ ] Large Boiler pattern in pattern library
- [ ] Boiler tick system
- [ ] Fuel burning (coal, charcoal, fluid fuel)
- [ ] Water input hatch → steam conversion
- [ ] Steam output hatch → PipeNetwork
- [ ] Overheat detection + damage
- [ ] Multi-size support (1×1×1 → 3×3×4)

---

## Раздел D: LCR (Large Chemical Reactor)

### Архитектура

**Pattern:** 3×3×3 casing, внутри air/fluid

**Компоненты:**
- Energy hatch
- Fluid input/output hatches
- Item input/output hatches
- Controller

**Рецепты:**
- Требуют жидкость + предметы
- Химические реакции: sulfuric_acid + iron → iron_sulfate
- Требуют EU/tick
- Побочные продукты (fluid output)

**Tick:**
1. RecipeManager: findRecipe(lcr_id, inputs + fluid_inputs)
2. Check energy available
3. Check fluid inputs in hatches
4. Process → consume inputs, produce outputs + fluid outputs
5. Persist state

### Критерии готовности

- [ ] LCR pattern in pattern library
- [ ] LCR tick system
- [ ] RecipeManager: fluid + solid input recipes
- [ ] Fluid hatch detection + management
- [ ] Byproduct handling (fluid output)
- [ ] Persistence via EntityStateStore

---

## Раздел E: Dissociation

### Что нужно сделать

При поломке ключевого блока мультиблока (controller или casing) — разрушить MultiblockController, очистить mb_id из meta-layer блоков, вернуть предметы из hatches.

### Flow

1. Client: breakBlockAction(pos)
2. SetBlockCASHandler: CAS → AIR
3. world.blocks.changed → SimulationCore
4. onBlockChanged(pos, AIR) → check: isMultiblockAnchor(pos)?
5. If anchor → destroy controller:
   - Remove ECS entity
   - Clear mb_id from ALL pattern blocks
   - Eject hatch contents
   - Publish sim.multiblock.destroyed
   - Client: remove multiblock visual

### Критерии готовности

- [ ] isMultiblockAnchor() — проверка по mb_id в meta-layer
- [ ] Dissociation cascade: anchor broken → full cleanup
- [ ] mb_id clear from all pattern blocks
- [ ] Hatch contents ejection
- [ ] Client: multiblock visual removal

---

## Сводные зависимости

```
SpatialIndex (L2 prerequisite)
    └── Pattern detection
        ├── EBF tick (heating coils, heat req)
        ├── Large Boiler tick (firebox, steam)
        ├── LCR tick (fluid+solid recipes)
        └── Dissociation (anchor → cleanup)
```

- RecipeManager — уже готов (ConditionEvaluator с MachineState)
- EntityStateStore — уже готов (сохранение MultiblockController)
- SpatialIndex — нужно реализовать (или defer в пользу прямой проверки паттерна)
- MachineSystem — уже готов (3-pass tick, managed_externally)
