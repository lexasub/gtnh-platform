# EPIC: Transport — Pipes & Cables

**Layer**: L2  
**Статус**: 🟡 **WIP — CableGraph built + wired; client mesh infra done**  
**Последнее обновление**: 2026-06-28 — Client: PipeMeshBuilder + CableMeshBuilder + ChunkMeshBuilder integration  
**Зависимости**: PipeNetwork (готов), MachineComponent.side_config (4-electric-tools-wrench ✅ DONE), item registry

## Userflow диаграммы

- `doc/userflow/06-item-energy-transport.puml` — T1: Item pipes, T2: Fluid pipes, T3: Energy cables
- `doc/userflow/07-ore-processing-chain.puml` — O2: Voltage tiers & transformers

## Обзор

Три вида транспорта между машинами:
1. **Item pipes** — перемещение предметов между инвентарями
2. **Fluid pipes** — перемещение жидкостей (fluids-as-items)
3. **Energy cables** — электрические провода с вольтажом, потерями, взрывами

---

## Раздел A: Item Pipes

### Что нужно сделать

Трубы для предметов: соединение между output-слотом одной машины и input-слотом другой.

### Где смотреть

| Файл | Роль |
|------|------|
| `src/services/pipe_network/PipeNetwork.h/.cpp` | PipeNetworkManager — BFS, energy/fluid distribution, node discovery |
| `src/services/pipe_network/PipeNetworkService.h/.cpp` | **isPipeBlock() = заглушка (всегда false)**, MessageRouter integration |
| `src/services/pipe_network/Client/MessageRouterClient.h/.cpp` | Клиент MessageRouter для pipe_network |
| `src/services/simulation_core/Network/PipeEnergyClient.h/.cpp` | Как SimulationCore вызывает PipeNetwork (topic-based RPC) |
| `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` | Использует PipeEnergyClient для energy consumption |
| `src/services/simulation_core/ECS/Systems/GeneratorSystem.cpp` | Публикует energy.node.update |
| `src/services/simulation_core/ECS/Systems/BoilerSystem.cpp` | Публикует energy.node.update |
| `data/registry/consumers.csv` / `producers.csv` | **Нет pipe/cable block_id** — только машины |

### Текущее состояние (из кода)

**PipeNetwork — energy routing system + CableGraph wired (2026-06-27):**
- ✅ BFS graph построен, `discoverNetwork()`, `rebuildNetworks()` работают
- ✅ `distributeEnergy()` распределяет энергию между consumers
- ✅ Топики: `energy.node.update`, `energy.check.request/response`, `energy.consume.request/response`, `energy.flow`
- ✅ SimulationCore интегрирован через PipeEnergyClient (MachineSystem, GeneratorSystem, BoilerSystem)
- ✅ **CableGraph**: `registerGenerator()`, `registerMachine()`, `registerCable()`, `removeCableNode()` — все методы реализованы
- ✅ **CableGraph wiring** (2026-06-27): `PipeNetworkService::handleNodeUpdate` вызывает registerGenerator/registerMachine для ELECTRICITY; GeneratorSystem публикует is_source=true для ELECTRICITY
- ⚠️ **CableGraph unregister** — `removeCableNode()` баг: erase по значению (cableNodeId) из map по ключу (entityId) — `m_generatorToCable.erase(it->second.id)` — не исправлено
- ❌ `isPipeBlock()` — заглушка: `return false` — нет pipe/cable block_id
- ❌ Item transport — **полностью отсутствует**
- ❌ Fluid transport — базовая поддержка (fluidId, fluidBuffer) но без pipe network
- ❌ Voltage tier checking — отсутствует
- ❌ Cable overheat/explosion — отсутствует
- ❌ Cable loss — отсутствует
- ❌ Transformers — отсутствуют
- ❌ Нет block_id для труб/кабелей в MachineRegistry

### Блоки

| Block ID | Название |
|----------|----------|
| TBD | item_pipe |
| TBD | fluid_pipe |
| TBD | dense_item_pipe |
| TBD | dense_fluid_pipe |

### Критерии готовности

- [ ] item_pipe block_id в MachineRegistry
- [ ] isPipeBlock() для item_pipe
- [ ] PipeNetwork BFS через item pipes (отдельный граф от energy)
- [ ] PushItemToPipe: машина → output pipe
- [ ] Item movement: 1 block/tick
- [ ] Insert into machine: pipe → input slot (machine с INPUT role)
- [ ] Item buffer persistence через ESS
- [x] **Client: pipe mesh builder** — `PipeMeshBuilder` с detectConnections + buildPipeMesh (интегрирован в ChunkMeshBuilder) — **✅ done**

---

## Раздел B: Fluid Pipes

### Что нужно сделать

Трубы для жидкостей (вода, пар, нефть). Жидкости как ItemStack — архитектурное решение принято.

### Архитектура

**Fluids as items:**
- Вода = item_id (например, 1000 = "water")
- Пар = item_id (например, 1001 = "steam")
- Рецепты проверяют fluid item_id так же как обычные предметы
- Отдельный FluidTankComponent не нужен

**Fluid graph:**
- PipeNetwork второй граф — для жидкостей
- BoilerSystem производит steam item_id
- Steam-машины потребляют steam item_id
- infinite source placeholder: вода из бесконечного источника

**Delivery:**
- Аналогично item pipes: BFS, 1 блок/тик
- Но жидкости могут течь быстрее (настраиваемая скорость)
- Давление: гравитация (жидкости текут вниз)

### Где смотреть

| Файл | Роль |
|------|------|
| `src/services/pipe_network/` | Fluid graph — расширение PipeNetwork |
| `src/services/simulation_core/ECS/Systems/BoilerSystem.h` | Производит steam |
| `src/services/simulation_core/ECS/Components/MachineComponent.h` | energy_out=STEAM, energy_in=STEAM |
| `src/protocol/core.fbs` | FluidTank (UI only) |
| `data/registry/items.csv` | fluid item_id |

### Критерии готовности

- [ ] fluid item_id в registries (water, steam, sulfuric_acid)
- [ ] PipeNetwork fluid graph (отдельный от energy)
- [ ] BoilerSystem → pipe → machine flow
- [ ] Infinite water source placeholder
- [ ] Gravity: fluid flows downward
- [ ] Client: fluid pipe visuals
- [ ] RecipeManager: fluid item_id в рецептах

---

## Раздел C: Energy Cables & Voltage

### Что нужно сделать

Электрические провода с вольтажными tier-ами, потерями, взрывами при перегрузке. Трансформеры для step up/down.

### Архитектура

**Voltage tiers (GregTech convention):**
```
ULV (0) =    8 EU/t    Tier 0
LV  (1) =   32 EU/t    Tier 1
MV  (2) =  128 EU/t    Tier 2
HV  (3) =  512 EU/t    Tier 3
EV  (4) = 2048 EU/t    Tier 4
IV  (5) = 8192 EU/t    Tier 5
LuV (6) = 32768 EU/t   Tier 6
ZPM (7) = 131072 EU/t  Tier 7
UV  (8) = 524288 EU/t  Tier 8
UHV (9) = 2097152 EU/t Tier 9
```

**Cable tiers:**
- Каждый кабель имеет tier (пропускная способность)
- Cable tier >= generator tier → OK
- Cable tier < generator tier → нагрев → взрыв
- Cable tier > machine tier → машина получает слишком много → взрыв
- Потери: energy_loss = distance * loss_per_block

**Transformers:**
- Step Up: MV(128) → HV(512), одна грань = high, 5 граней = low
- Step Down: HV(512) → MV(128), одна грань = high, 5 граней = low
- Трансформер не даёт машине взорваться при несоответствии tier

### Где смотреть

| Файл | Роль |
|------|------|
| `src/services/pipe_network/` | Energy graph — расширить: проверка tier |
| `src/services/pipe_network/PipeNetworkService.h` | isPipeBlock() — добавить cable block_id |
| `src/services/pipe_network/pipe_network.fbs` | Voltage tier в EnergyNode |
| `src/protocol/core.fbs` | EnergyType — добавить tier? |
| `src/services/simulation_core/ECS/Components/EnergyStorage.h` | tier, maxInput, maxOutput |
| `data/registry/items.csv` | cable block_id per tier |

### Блоки

| Block ID | Название | Tier |
|----------|----------|------|
| TBD | cable_tin | LV (1) |
| TBD | cable_copper | LV (1) |
| TBD | cable_gold | MV (2) |
| TBD | cable_alu | MV (2) |
| TBD | cable_tungsten | HV (3) |
| TBD | cable_platinum | EV (4) |
| TBD | transformer_mv_hv | MV↔HV |
| TBD | transformer_hv_ev | HV↔EV |

### Критерии готовности

- [x] **CableGraph класс** (registerGenerator, registerMachine, registerCable, removeCableNode, removeCable, hasConnection, findPath, getConnectedBlocks) — **✅ exists**
- [x] **CableGraph wiring** — registerGenerator/registerMachine вызываются из PipeNetworkService::handleNodeUpdate для ELECTRICITY; GeneratorSystem публикует ELECTRICITY как source — **✅ DONE**
- [ ] Cable block_id per tier в MachineRegistry
- [ ] isCableBlock() в PipeNetworkService
- [ ] Voltage tier checking при BFS routing
- [ ] Cable overheat → explosion при превышении tier
- [ ] Cable loss: energy_loss = distance * loss_per_block
- [ ] Transformer block: step up/down, face config
- [x] **Client: cable mesh builder** — `CableMeshBuilder` с tier-based цветами + detectConnections (интегрирован в ChunkMeshBuilder) — **✅ done**
- [ ] Client: overvoltage warning UI (not started)
- [ ] Client: explosion effect при перегрузке

---

## Сводные зависимости

```
MachineComponent (side_config)
    └── PipeNetwork BFS routing
            ├── Item pipes (PushItemToPipe)
            ├── Fluid pipes (fluid graph)
            └── Energy cables (voltage check)
                    └── Transformer (step up/down)
```

- PipeNetwork — готов для energy graph + CableGraph работает
- MachineComponent.side_config — ✅ DONE (4-electric-tools-wrench)
- item/fluid/cable block_id — нужно добавить в MachineRegistry
- BFS routing для разных типов — отдельные графы или один с type filter
