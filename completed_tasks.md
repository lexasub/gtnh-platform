

## 🟢 Item/Fluid/Energy Transport Network — Вторая волна: интеграция

**Scope:** Подключение TransformerSystem, FluidClient и fluid-топиков в SimulationCore.

### Что сделано

- **FlatBuffers headers regenerated** — flatc сгенерировал Item-типы (ItemNodeUpdate, ItemTransferReq/Resp, ItemFlowEvent) для всех сервисов
- **TransformerSystem** зарегистрирован в SimulationEngine (step-up/down MV↔HV, HV↔EV)
- **FluidClient** создан и подключен в main.cpp
- **fluid.flow handler** — SimCore обрабатывает fluid.flow (списывает жидкость у источника)
- **fluid.consume.response handler** — логирование результата потребления жидкости
- **Подписки** — SimCore подписан на fluid.flow, fluid.consume.response, energy.check.response

### Файлы
| Файл | Изменение |
|------|-----------|
| `src/services/simulation_core/main.cpp` | +TransformerSystem, FluidClient, fluid-хендлеры, fluid-подписки |
| `*pipenet_fbs/pipe_network_generated.h` | Regenerated (Item-типы) |
| `*simcored_fbs/pipe_network_generated.h` | Regenerated (Item-типы) |
| `*game_client_fbs/pipe_network_generated.h` | Regenerated (Item-типы) |

---

## 🟡 Item/Fluid/Energy Transport Network — critical gap closed

**Scope:** Реализована базовая инфраструктура транспорта предметов, жидкости и энергии через PipeNetwork.

### Что сделано

**Протокол (pipe_network.fbs):**
- Добавлены ItemNodeUpdate, ItemTransferReq/Resp, ItemFlowEvent таблицы
- Документированы топики для всех транспортных типов (energy, fluid, item)

**Item Pipe Network:**
- Реализованы `moveItemsInNetwork()` — BFS-перемещение предметов от source к sink
- Реализована `tickItemNetworks()` — периодический тик всех item-сетей
- Реализован `findNextItemHop()` — поиск следующего узла для маршрутизации

**Fluid Network:**
- Создан `FluidClient.h/.cpp` для SimulationCore (аналог PipeEnergyClient)
- Добавлены fluid-хендлеры в PipeNetworkService (handleFluidNodeUpdate/Check/Consume)
- PipeNetworkService подписан на fluid.node.update, fluid.check.request, fluid.consume.request
- FluidRegistry инициализирован дефолтными жидкостями (water, steam, sulfuric_acid)

**Cable Network:**
- Уже существовал CableGraph с полной маршрутизацией (voltage, ampacity, overheat)
- PipeNetworkService.tick() вызывает cable_graph_.tick() и network_manager_.tickItemNetworks()

**Transformers:**
- Создан TransformerComponent (entity component для блоков 72-73)
- Создан TransformerSystem с step-up/step-down логикой (MV→HV, HV→EV)

**Баги:**
- Исправлен PipeMeshBuilder.h: pipeTypeToBlockId() возвращал неверные ID
  - ITEM_PIPE→64 → 62, FLUID_PIPE→65 → 61, кабели 80-85 → 66-71

### Файлы
| Файл | Действие |
|------|----------|
| `src/protocol/pipe_network.fbs` | +Item transport tables |
| `src/services/pipe_network/PipeNetwork.cpp` | +moveItemsInNetwork/tickItemNetworks/findNextItemHop |
| `src/services/pipe_network/PipeNetworkService.h` | +fluid handler declarations |
| `src/services/pipe_network/PipeNetworkService.cpp` | +fluid subs, handlers, tick integration |
| `src/services/pipe_network/FluidRegistry.h` | +constructor, initDefaults |
| `src/services/pipe_network/FluidRegistry.cpp` | +water/steam/acid registration |
| `src/services/simulation_core/Network/FluidClient.h` | NEW |
| `src/services/simulation_core/Network/FluidClient.cpp` | NEW |
| `src/services/simulation_core/ECS/components/TransformerComponent.h` | NEW |
| `src/services/simulation_core/ECS/Systems/TransformerSystem.h` | NEW |
| `src/services/simulation_core/ECS/Systems/TransformerSystem.cpp` | NEW |
| `src/services/game_client/rendering/PipeMeshBuilder.h` | fixed block IDs |

### Остаётся
- Regenerate FlatBuffers headers (`flatc`) — требуется для Item протокола
- Подключить FluidClient/TransformerSystem в SimulationCore main.cpp
- Client-side визуализация соединения труб

---

## 🔥 P0: Double block processing — правильный фикс

**Проблема:** ChunkStore сам обрабатывает `player.actions` — ставит блок и публикует `world.blocks.changed`. SimulationCore делает то же самое через RPC. Блок ставится дважды, meta затирается.

**Бизнес-логика — за SimulationCore.** ChunkStore — тупое хранилище.

**Фикс:** Убрать обработку PLACE/BREAK из ChunkStore. Оставить только CHUNK_REQUEST.

**Что изменить в `src/services/chunk_store/Network/RouterClient.cpp`:**
- Удалить блок `BREAK` (строки ~127-135) из `onPublish`
- Удалить блок `PLACE` (строки ~137-148) из `onPublish`
- Оставить только `CHUNK_REQUEST` (строки ~149-161)
- Подписка `subscribe("player.actions")` остаётся — ChunkStore всё ещё обрабатывает CHUNK_REQUEST

---

## 🔥 P0: Архитектурная основа — CAS SetBlock + optimistic ack

Решение принято. Дальнейшие задачи опираются на эту архитектуру.

### Data flow

```
Client                                          SimCore                  ChunkStore
  │                                               │                        │
  │── SetBlockAction(pos, expected, new) ────────►│                        │
  │                                               │                        │
  │                                               │  ┌──────────────────┐  │
  │                                               │  │ Бизнес-логика:   │  │
  │                                               │  │ проверка прав,   │  │
  │                                               │  │ можно ли ставить,│  │
  │                                               │  │ подмена блока    │  │
  │                                               │  │ (трансформация,  │  │
  │                                               │  │  мультиблок...)  │  │
  │                                               │  └───────┬──────────┘  │
  │                                               │          │              │
  │                                               │  ┌───────▼──────────┐  │
  │                                               │  │ fail?            │  │
  │◄── BlockAck(REJECTED, reason) ────────────────┤  │ → BlockAck      │  │
  │                                               │  │   REJECTED       │  │
  │                                               │  └───────┬──────────┘  │
  │                                               │          │ ok          │
  │                                               │          ▼             │
  │                                               │── RPC SetBlockCAS ────►│
  │                                               │   (pos, expected,     │
  │                                               │    final_id, meta)    │
  │                                               │                        │
  │                                               │  ┌──────────────────┐  │
  │                                               │  │ flat array:      │  │
  │                                               │  │ blocks[idx] ==  │  │
  │                                               │  │ expected?       │  │
  │                                               │  │   Y→ set final  │  │
  │                                               │  │   N→ skip       │  │
  │                                               │  │ всегда возвр.   │  │
  │                                               │  │ актуальный блок │  │
  │                                               │  └──────────────────┘  │
  │                                               │◄── CAS result ─────────┤
  │                                               │    (OK/CONFLICT +      │
  │                                               │     actual_id,meta)    │
  │                                               │                        │
  │◄── BlockAck ──── (асинхронно) ────────────────┤                        │
  │    COMMITTED(pos, final_id, meta)             │                        │
  │      или                                      │                        │
  │    CONFLICT(pos, actual_id, actual_meta)      │                        │
```

### CAS SetBlock — что это

ChunkStoreService получает RPC: `SetBlockCAS(x, y, z, expected_block_id, new_block_id, meta)`

Внутри ChunkStore (один I/O поток / atomic op):
```cpp
// flat array, O(1)
auto current = blocks[idx];
if (current == expected_block_id) {
    blocks[idx] = new_block_id;
    meta[idx] = meta;
    return {CAS_OK, new_block_id, meta};
} else {
    // всегда возвращаем актуальное состояние, не просто ошибку
    return {CAS_CONFLICT, current, meta[idx]};
}
```

**BlockAck несёт финальный block_id, а не запрошенный:**

| Статус | Что значит | Клиент делает |
|--------|-----------|---------------|
| `REJECTED(pos, reason)` | SimCore отклонил — бизнес-логика не пропустила (нет прав, нельзя ставить, блок не подходит). CAS не делался | Откатывает optimistic рендер |
| `COMMITTED(pos, block_id, meta)` | Блок живёт на сервере. `block_id` может отличаться от того что просил клиент — SimCore подменил по бизнес-логике до CAS (мультиблок, трансформация) | Синхронизирует мир по `block_id` |
| `CONFLICT(pos, block_id, meta)` | Блок не поставлен — CAS не прошёл (кто-то изменил блок раньше). `block_id` = актуальное состояние на сервере | Откатывает optimistic рендер, показывает реальный блок |

Клиент в обоих случаях делает одно и то же: **кладёт `block_id` из BlockAck в свой мир**. Разница только в UX — CONFLICT можно подсветить красным.

**Подмена в SimCore — до CAS.** ChunkStore получает уже финальный `(final_id, meta)` и делает CAS с ним. Никакого двойного RPC.

Не надо слать отдельный GetBlock — CAS + BlockAck сами возвращают актуальное состояние. Клиент всегда синхронизирован.

**Почему это дешево:**
- flat array — `blocks[idx]` без кэш-промаха
- один поток на модификацию (I/O pool) — не нужна блокировка
- один RPC вместо `GetBlock + SetBlock`
- не читает LMDB (работает из кэша)

### Роли

| Компонент | Делает |
|-----------|--------|
| **Client** | Шлёт `SetBlockAction(pos, expected, new)` |
| **Gateway** | Фасад — форвардит сообщения |
| **SimulationCore** | Валидирует, **подменяет блок по бизнес-логике** (до CAS), шлёт `SetBlockCAS` с финальным `final_id`, отвечает клиенту асинхронно |
| **ChunkStore** | Тупое хранилище — CAS на flat array, возвращает актуальное состояние, публикует `world.blocks.changed` |

### Что даёт

- **Атомарность** — проверка и запись за один RPC
- **Optimistic ack** — клиент сразу знает что действие принято
- **Нет race** — ChunkStore решает кто первый
- **SimCore не хранит чанки** — не дублирует состояние

### Что меняется в протоколе

**PlayerAction** (уже есть `block_id`, `face` → переиспользуем):
- `current_block_id`: uint16 — что клиент "видит" сейчас на этой позиции
- `new_block_id`: uint16 — что хочет поставить/сломать (0 = сломать)
- `pos`: Vec3i
- `action_type`: PLACE/BREAK/CLICK/CHUNK_REQUEST/...

**BlockAck** (новое сообщение):
- `pos`: Vec3i
- `status`: ACCEPTED | COMMITTED | CONFLICT | REJECTED

---

## Session: basic-mechanics UI — Tasks 8, 9, 12 + FindOrCreate fix

- **Task 8 (ServerLogic mocks):** Created `IMechanism.h`, `MockFurnace.h`, `MockMacerator.h`, `MockCompressor.h` with inline `Tick()`, `BlockType` enum usage, and energy initialization.
- **Task 9 (BlockType consolidation):** Created `Common/BlockType.h`, updated `BlockUIFactory.h/cpp`, `MachineWindow.h/cpp`, `IMechanism.h`, and all mocks to use `BlockType` instead of `uint16_t`.
- **Task 12 (NEI-style side panels):** Created `UI/Core/ISidePanel.h`, `UI/Panels/RecipePanel.h`, wired panels registry into `UIManager.h/cpp` and `UIDefaults.cpp`.
- **FindOrCreate fix:** Implemented missing `BlockUIFactory::FindOrCreate<T>()` template and `BlockUIFactory::FindOrCreateMachine()` in `BlockUIFactory.h/cpp`.
- **Workbench lambda fix:** Fixed lambda to avoid binding temporary to non-const ref.
- **Mock energy fix:** Fixed mock constructors to initialize `energy_ = maxEnergy_` so machines actually tick.
- **ItemStack fix:** Replaced non-existent `IsEmpty()`, `IsItemSameAs()`, `IsFull()` calls with direct field access in all mocks.
- **MachineWindow.cpp fix:** Added missing `#include "UI/Components/SlotGrid.h"` and passed `SlotStyle` to `RenderSlot()` calls.
- **UIDefaults fix:** Updated `TryOpenBlockUI()` signature to accept `BlockType`, added `ToBlockType()` conversion in `GameClient.cpp`.
- **PlayerInventory fix:** Added missing constructor definition to fix linker error.

---
