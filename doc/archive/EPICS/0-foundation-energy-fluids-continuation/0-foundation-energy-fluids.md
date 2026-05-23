# Энергия и жидкости — Continuation

**Эпик**: Foundation — Energy & Fluids (Continuation)  
**Слой**: L1 (завершение) → L2 (отложено)  
**Статус**: Continuation spec

> **Update 2026-06**: SimulationCore→PipeNetwork интеграция выполнена (energy.node.update, energy.consume.request/response, energy.flow). PipeNetwork подключён к MessageRouter. Детали — в ROADMAP.md и AGENTS.md.

## Что уже сделано (см. archive)

L1 primitives реализованы:
- EnergyStorage ECS компонент ✅
- MachineSystem потребляет энергию при обработке рецептов ✅
- PipeNetwork — библиотека графов (BFS, distributeEnergy/Fluid) ✅
- FluidTank/FluidStack/MachineFluidTank протоколы ✅

**Полный список** — в `doc/EPICS/archive/0-foundation-energy-fluids/REMAINING_WORK.md`.

---

## Affected Services

| Service | Layer | Role |
|---------|-------|------|
| **SimulationCore** | L1 | ECS энергия, интеграция PipeNetwork, генерация энергии |
| **PipeNetwork** | L1 | Подключение к MessageRouter, RPC для SimulationCore |
| **ChunkStore** | L0 | Блоки-провода/трубы (meta-слой для соединений) |
| **Gateway** | L0 | Релей энергетических/жидкостных обновлений |
| **GameClient** | L1 | Отображение энергии/жидкости в MachineWindow |
| **EntityStateStore** | L0 | Сохранение EnergyStorage и FluidTank состояния |

> **Architecture rule**: PipeNetwork — самостоятельный сервис, доступный через MessageRouter. SimulationCore вызывает его RPC для расчёта потоков, но не владеет графом.

---

## L1 Remaining — PipeNetwork интеграция

### PipeNetwork: интеграция с MessageRouter ✅ (частично)

**Статус**: PipeNetwork подключён к MessageRouter (main.cpp: MessageRouterClient + io_context loop). Имеет подписки и обработчики для энергетических топиков.

**Подписки** (текущие):
- `energy.node.update` — обновление состояния узлов сети
- `energy.consume.request` — запрос на потребление энергии от SimulationCore
- `energy.check.request` — проверка доступности энергии

**Публикации** (текущие):
- `energy.consume.response` — ответ на запрос потребления
- `energy.flow` — событие потока энергии между узлами

**Ещё нужно**:
- [x] Подписка на `world.blocks.changed` для автоматического обнаружения труб — ✅ **DONE** (PipeNetworkService.h/.cpp: `isPipeBlock()` + `world.blocks.changed` subscription)
- [ ] Подписка на `pipe.network.query` / `pipe.network.distribute` (если понадобится) — deferred

### PipeNetwork: интеграция с SimulationCore ✅

**Статус**: SimulationCore интегрирован с PipeNetwork через MessageRouter. Реализован асинхронный consume request/response цикл.

**Что работает**:
1. SimulationCore публикует `energy.node.update` для каждого узла при изменении энергии (GeneratorSystem, BoilerSystem, MachineSystem)
2. Когда MachineSystem не хватает энергии, он отправляет `energy.consume.request` в PipeNetwork
3. PipeNetwork отвечает `energy.consume.response` (consumed, remaining)
4. SimulationCore применяет consumed к EnergyStorage в ECS
5. PipeNetwork публикует `energy.flow` при движении энергии
6. SimulationCore обрабатывает flow → deduct-ит энергию у источника

**Архитектурное решение**: PipeNetwork — **отдельный сервис**. SimulationCore вызывает его через MessageRouter RPC для расчёта энергетических потоков. EnergyStorage остаётся ECS-компонентом (буфер машины), но движение энергии считает PipeNetwork.

### Жидкости: ECS компоненты — deferred (fluids as items)

**Решение**: Жидкости, газы, плазма — **обычные предметы** (ItemStack с fluid/gas/plasma item_id). Отдельный `FluidTankComponent` не нужен. MachineInventory может содержать fluid item_id так же как и обычные предметы.

Для MVP жидкости не требуют ECS-компонентов. Загрузка жидкостей через рецепты (input = fluid item) — как и любой другой предмет.

### Жидкости: MachineGUI отображение — deferred (fluids as items)

Жидкости как ItemStack — отображаются в обычных слотах. Для PipeNetwork жидкостные флоу — как отдельный тип графа (связка "PipeNetwork для жидкостей + infinite source placeholder для автономных машин").

### Жидкости: RecipeManager — жидкостные рецепты

**Статус**: ConditionEvaluator уже получает реальное MachineState из ECS (temperature, purity, biome, energy — см. `RecipeManager.cpp` в simulation_core). Единственный gap — fluid_slots, но это решено (fluids as items).

**Решение**: Для жидкостных рецептов — обычные ItemStack проверки. Не требует MachineState изменений.

---

## L2 — Отложено полностью

- Провода (Cable блоки с потерями, напряжением, током)
- Электрические сети с генераторами→проводами→машинами (PipeNetwork-граф)
- Трубы для жидкостей (автоматическое соединение, давление, типы жидкостей)
- Трансформеры напряжения
- Отображение энергосети на клиенте

---

## Протокол (дополнения)

### BlockEntityUpdate (обновлён — core.fbs)

Таблица расширена для поддержки машин, мультиблоков, жидкостей, хэтчей и обложек:
- Simple machines: pos, machine_type, progress, energy, energy_capacity, input_items, output_items
- Fluids: fluid_tanks[FluidTank]
- Multiblocks: mb_id, structure_valid, hatches[HatchInfo], covers[CoverInfo], network_id
- Номер типа 10 в GatewayPayload (gateway.fbs)

### EnergyDistribution (не реализован)

> **Note**: Этот протокол не использован. Вместо него реализован механизм energy.consume.request/response + energy.flow (см. pipe_network.fbs).
```flatbuffers
table EnergyDistribution {
    network_id: uint64;
    node_deltas: [EnergyDelta];
}
table EnergyDelta {
    node_id: uint64;
    energy_delta: int32;
}
```

### PipeNetworkState (новый)
```flatbuffers
table PipeNetworkState {
    networks: [NetworkInfo];
}
table NetworkInfo {
    id: uint64;
    node_count: uint32;
    total_energy: int32;
    total_fluid: int32;
    fluid_id: uint32;
}
```

---

## Критерии готовности L1

- [x] MachineSystem расходует энергию (работает с начала)
- [x] **PipeNetwork** — отдельный сервис с MessageRouter (подключён, обрабатывает энергетические топики)
- [x] SimulationCore вызывает PipeNetwork RPC для распределения энергии (energy.consume.request/response + energy.node.update + energy.flow)
- [x] ConditionEvaluator уже получает реальное MachineState (gap закрыт)
- [x] **CreativeGenerator** (configurable, ID 63) добавляет 1024 EU/tick в EnergyStorage (CreativeGeneratorSystem)
- [x] Жидкости как ItemStack в обычных слотах (без FluidTankComponent) — решение принято, архитектура определена. Отдельного тикета не требуется.

---

## Зависимости

- `PipeNetwork` — отдельный сервис, интеграция через MessageRouter
- `2-infrastructure-shared-recipe-lib` — ✅ уже в архиве (рефакторинг выполнен)
