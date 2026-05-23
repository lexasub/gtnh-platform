# Open Questions

Сводка нерешённых / решённых архитектурных и дизайнерских вопросов.

Статусы:
- ✅ **Resolved** — решение принято
- 🔷 **Grooming** — требует уточнения/обсуждения
- ⏳ **Post-MVP** — отложено

---

## Energy

### ✅ Q1. Кто наполняет EnergyStorage?

**Решение**: **CreativeGenerator (configurable)**.

`MachineSystem::tick()` потребляет `energy.current` — для тестирования и творческого режима добавляем CreativeGenerator, который даёт настраиваемое количество EU/tick. Позже генераторы будут настоящими (солнечные, паровые и т.д.).

**Контекст**: SimulationCore ECS MachineSystem.cpp — consumption есть, production нет.

### ✅ Q2. Energy — локальный ECS-компонент или PipeNetwork-граф?

**Решение**: **PipeNetwork рассчитывает энергетический поток**.

EnergyStorage остаётся ECS-компонентом (буфер машины), но движение энергии между генератором и потребителем считает PipeNetwork. Интеграция: PipeNetwork как отдельный сервис получает данные из SimulationCore, решает граф, публикует результат.

---

## Fluids

### ✅ Q3. FluidSlots в MachineState — зачем выключены?

**Решение**: **Не нужно отдельного поля**. Газы, жидкости, плазма — всё будет как обычные предметы (ItemStack с fluid/gas/plasma item_id). MachineState не требует `fluid_slots`.

### ✅ Q4. FluidTank/FluidStack в `.fbs` — не используются в C++

**Решение**: **Оставить в протоколе**. FluidTank нужен для отображения ёмкости при просмотре бака (в UI). FluidStack может использоваться в BlockEntityUpdate.fluid_tanks[] для машин с жидкостями.

---

## Networking / Architecture

### ✅ Q5. PipeNetwork — отдельный service или библиотека для SimulationCore?

**Решение**: **Отдельный сервис**.

PipeNetworkManager — самостоятельный процесс с Asio TCP + MessageRouter. SimulationCore шлёт events при изменении pipe-блоков/energystorage, PipeNetwork пересчитывает граф и публикует результат.

### ✅ Q6. SpatialIndex — stub, нужен ли на L1?

**Решение**: **Не нужен на L1 (вариант 3)**. Simple Machines (furnace, macerator, compressor) не требуют пространственного поиска. Multiblocks deferred до L2, SpatialIndex реализуется вместе с ними.

### ✅ Q7. MetaDB login/logout flow — финализирован ли?

**Решение**: **Уже реализован end-to-end**:
1. Gateway публикует `player.joined` при первом PlayerAction (gateway.cpp:398-404)
2. Gateway публикует `player.left` с последней известной позицией при дисконнекте (gateway.cpp:132-141)
3. MetaDB подписан на оба топика (router_client.go:103-104)
4. MetaDB сохраняет/загружает позицию (db.go: SavePlayerPosition/GetPlayerPosition)
5. Таблица players в SQLite создана (db.go:18-23)

---

## Protocol

### ✅ Q8. BlockEntityUpdate — отдельное сообщение или расширение EntitySnapshot?

**Решение**: **Отдельное FlatBuffers-сообщение** в `core.fbs`. Добавлены поля для:
- Простых машин: pos, machine_type, progress, energy, energy_capacity, input_items, output_items
- Машин с жидкостями: fluid_tanks[FluidTank]
- Мультиблоков: mb_id, structure_valid, hatches[HatchInfo], covers[CoverInfo], network_id
- Обложки (covers[CoverInfo]) и хэтчи (hatches[HatchInfo]) — с самого начала, пустые для L1

FlatBuffers схема обновлена в `core.fbs:267-312`.

### ⏳ Q9. Inventory Actions — breaking change для wire protocol?

**Решение**: **Post-MVP**. Luanti-паттерны с `InventoryAction` — после того, как базовый инвентарь работает стабильно. Решение очевидно, внедрение позже.

---

## Gameplay

### ✅ Q10. Recipe auto-selection при нескольких рецептах на один input

**Решение**: **UI (base modes, как GTNH)**. Если в машину подходят несколько рецептов — показываем интерфейс выбора (как в GregTech: режимы работы, приоритеты). Игрок выбирает.

### ✅ Q11. Передача жидкости между машинами — PipeNetwork или Block соседи?

**Решение**: **PipeNetwork (2) + infinite source placeholder (3)**. Реальный флоу жидкостей — через PipeNetwork-граф. Для разработки/тестирования — бесконечный источник внутри машины (как вода из ниоткуда).

---

## Cross-cutting (deferred)

### 🔷 Q12. SimulationCore — single-threaded tick хватит?

**Статус**: **Grooming on 100+ machines**. Когда дойдёт до сотен машин — тогда и решать.

### 🔷 Q13. ImGui state sync — с какой частотой обновлять GUI?

**Статус**: **Grooming on 100+ machines**. Проблема производительности синка — когда будут реальные машины для профилирования.

---

## Статистика

| Статус | Вопросы |
|--------|---------|
| ✅ Resolved | Q1, Q2, Q3, Q4, Q5, Q6, Q7, Q8, Q10, Q11 |
| 🔷 Grooming | Q12, Q13 |
| ⏳ Post-MVP | Q9 |

