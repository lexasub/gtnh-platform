# Energy System + Machine Registry — Design

## Motivation

Убрать хаос из трёх несинхронизированных идентификаторов машин (BlockType enum на клиенте,
BlockType enum в simcore, MachineType enum в recipe.fbs) и заменить единым source of truth —
`block_id` из `items.csv`. Ввести понятие EnergyType (HEAT, STEAM, ELECTRICITY) в протокол
и simcore, чтобы клиент корректно отображал энергию, а сервер — распределял по типам.

---

## Source of Truth: два CSV-файла

Машины разделены на потребителей и производителей энергии.

### `data/registry/consumers.csv`

Потребляют энергию из буфера/сети, чтобы крафтить.

```csv
id,name,class,energy_in,tier,slots_in,slots_out
36,gtnh:heat_furnace,Furnace,HEAT,0,1,1
48,gtnh:heat_macerator,Macerator,HEAT,0,1,1
51,gtnh:steam_macerator,Macerator,STEAM,0,1,1
52,gtnh:steam_compressor,Compressor,STEAM,0,1,1
60,gtnh:bronze_alloy_smelter,AlloySmelter,STEAM,0,2,1
61,gtnh:steam_extractor,Extractor,STEAM,0,1,1
62,gtnh:steam_mixer,Mixer,STEAM,0,2,1
```

### `data/registry/producers.csv`

Производят энергию во внешний буфер/сеть. Могут потреблять другой тип энергии на входе.

```csv
id,name,class,energy_out,energy_in,tier,slots_in,slots_out
46,gtnh:heat_generator,Generator,HEAT,,0,1,0
49,gtnh:steam_solid_boiler,Generator,STEAM,,0,2,1
50,gtnh:steam_heat_boiler,Generator,STEAM,HEAT,0,1,1
```

### Описание колонок

| Колонка | Где | Смысл |
|---------|-----|-------|
| `id` | оба | block_id из `items.csv` |
| `name` | оба | полное имя из `items.csv` (неймспейс: `gtnh:`) |
| `class` | оба | функциональный класс машины: `Furnace`, `Macerator`, `Compressor`, `Generator`... Используется ECS для выбора обработчика |
| `energy_in` | consumers | тип энергии, который машина потребляет из буфера для крафта |
| `energy_out` | producers | тип энергии, который машина производит в буфер |
| `energy_in` | producers | опционально: тип энергии, который машина потребляет на входе (гибриды вроде steam_heat_boiler) |
| `tier` | оба | 0=ULV, 1=LV, 2=MV ... |
| `slots_in` | оба | число входных слотов для предметов |
| `slots_out` | оба | число выходных слотов для предметов |

### Принцип

- **`energy_in`** — энергия, которая заполняет `EnergyStorage` машины и тратится на работу
- **`energy_out`** — энергия, которую машина генерирует в свой `EnergyStorage` (для передачи потребителям)
- Если у producers заполнен `energy_in` — машина-гибрид: потребляет один тип, выдает другой

Пополняется строкой в соответствующем CSV — нигде больше ничего менять не надо.

---

## Protocol FlatBuffers

### `protocol/core.fbs` — новый enum + изменения

```fbs
enum EnergyType : uint8 {
  ELECTRICITY = 0,
  HEAT = 1,
  STEAM = 2,
}
```

В `BlockEntityUpdate` добавить `energy_type`:

```fbs
table BlockEntityUpdate {
  pos:Vec3i (required);
  machine_type:uint16;             // block_id из items.csv
  progress:float;
  energy:uint32;
  energy_capacity:uint32;
  energy_type:EnergyType = ELECTRICITY;  // NEW
  // ... остальные поля без изменений
}
```

### `protocol/recipe.fbs` — MachineType → machine_id

```fbs
// enum MachineType — УДАЛЯЕТСЯ
// Везде где был MachineType, теперь uint16 (block_id)

table CheckRecipeReq {
  container:Container;
  machine_id:uint16;       // было machine_type:MachineType
}
```

Recipe в RecipeManager хранит `uint16_t machine_id` вместо `Protocol::MachineType`.

---

## C++ MachineRegistry (data-слой)

Общий класс для клиента и simcore. Загружает оба CSV рантайм.

```cpp
// src/libs/machine_registry/MachineRegistry.h
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include <optional>

enum class EnergyType : uint8_t {
    ELECTRICITY = 0,
    HEAT = 1,
    STEAM = 2,
};

enum class MachineRole : uint8_t {
    CONSUMER = 0,  // потребляет energy_in для крафта
    PRODUCER = 1,  // производит energy_out (опционально +energy_in)
};

struct MachineInfo {
    uint16_t id;
    std::string name;           // "gtnh:steam_macerator"
    std::string machine_class;   // "Macerator", "Furnace", "Generator"...
    MachineRole role;
    EnergyType energy_in;        // для CONSUMER: что жрёт; для PRODUCER: опц. вход
    std::optional<EnergyType> energy_out;  // только для PRODUCER: что выдаёт
    int tier;
    int slots_in;
    int slots_out;
};

class MachineRegistry {
public:
    static std::unique_ptr<MachineRegistry> Load(const char* consumers_path,
                                                  const char* producers_path);

    const MachineInfo* Get(uint16_t block_id) const;
    bool IsMachine(uint16_t block_id) const;
    bool IsConsumer(uint16_t block_id) const;
    bool IsProducer(uint16_t block_id) const;
    const std::unordered_map<uint16_t, MachineInfo>& All() const;

    static const char* EnergyLabel(EnergyType et);
};
```

---

## Simcore: ECS Components

### `MachineComponent` — замена machine_type на machine_id

```cpp
struct MachineComponent {
    uint16_t machine_id = 0;          // block_id из items.csv
    uint32_t x = 0, y = 0, z = 0;    // положение в мире
    uint64_t machine_uid = 0;         // уникальный ID entity
};
```

### `EnergyStorage` — добавляем EnergyType (роль — через тег-компонент)

```cpp
struct EnergyStorage {
    int32_t capacity;
    int32_t current;
    int32_t maxInput;
    int32_t maxOutput;
    int32_t tier;
    EnergyType type = EnergyType::ELECTRICITY;

    int32_t addEnergy(int32_t amount);
    int32_t consumeEnergy(int32_t amount);
    bool isEmpty() const;
    bool isFull() const;
};
```

Роль машины (источник / потребитель) НЕ хранится в EnergyStorage — это ECS-тег-компонент.

### `EnergySource` — тег-компонент для производителей энергии

```cpp
struct EnergySource {
    // пустой тег — machines with PRODUCER role get this
};
```

Любая entity с `EnergySource` — источник энергии. Система `EnergyDistributionSystem` делает `view<EnergyStorage, EnergySource>` для источников и `view<EnergyStorage>` (без `EnergySource`) для потребителей. Никаких `if` внутри цикла.

Гибрид `steam_heat_boiler`:
- `EnergyStorage(STEAM)` + `EnergySource` — производит пар
- `HeatIntakeComponent` — отдельный компонент, читается системой `HeatTransferSystem` (не EnergyDistributionSystem)

```cpp
struct HeatIntakeComponent {
    // Только для машин, которые принимают HEAT на вход
    // (например, steam_heat_boiler: HEAT in → STEAM out)
    EnergyType input_type = EnergyType::HEAT;
};
```

Расширяемость по тегам:
- `EnergySink` — принудительный сток (heat vent)
- `EnergyCapacitor` — буфер с повышенным tier
- и т.д.

Всё это пустые/минимальные struct — zero overhead в EnTT.

### `MachineFactory` — создание entity по block_id + role

```cpp
class MachineFactory {
public:
    MachineFactory(const MachineRegistry& registry);

    entt::entity Create(entt::registry& reg,
                        uint16_t block_id,
                        int32_t x, int32_t y, int32_t z);

private:
    const MachineRegistry& registry_;
};
```

`Create()`:
1. Ищет `MachineInfo` по block_id
2. Создаёт entity
3. Добавляет `MachineComponent`, `EnergyStorage(type, tier)`, `InventoryContainer(slots_in+slots_out)`, `RecipeProgress`
4. Если `role == PRODUCER` → добавляет `EnergySource` (тег)
5. Если у producers есть `energy_in` (гибрид) → добавляет `HeatIntakeComponent(input_type = energy_in)`

### `MachineSystem::tick()` + `EnergyDistributionSystem` — разделение через теги

`MachineSystem` обрабатывает **только CONSUMER**:

```cpp
// CONSUMER: entity имеет EnergyStorage, НО НЕ имеет EnergySource
auto view = reg_.view<MachineComponent, RecipeProgress, InventoryContainer, EnergyStorage>(entt::exclude<EnergySource>);
for (auto ent : view) {
    auto& machine = view.get<MachineComponent>(ent);
    auto& energy = view.get<EnergyStorage>(ent);
    auto* recipe = recipes_->findRecipeByInputs(machine.machine_id, inputItems);
    // ... generic craft, consume energy.current ...
}
```

`EnergyDistributionSystem` — распределяет энергию от источников к потребителям:

```cpp
// PRODUCER: entity с EnergySource
auto producers = reg_.view<EnergyStorage, EnergySource>();
// CONSUMER: entity без EnergySource
auto consumers = reg_.view<EnergyStorage>(entt::exclude<EnergySource>);
// Никаких if внутри цикла — чистые view
```

`GeneratorSystem` (post-MVP) — жжёт топливо, заполняет `EnergyStorage` у PRODUCER:

```cpp
// GeneratorSystem::tick():
// - Жжёт топливо (рецепт)
// - Заполняет EnergyStorage по energy_out
// - Если есть HeatIntakeComponent — проверяет наличие входной энергии
//   (через запрос к HeatTransferSystem или прямое чтение TemperatureComponent)
```

### `IEventPublisher` — новый параметр

```cpp
virtual void publishBlockEntityUpdate(
    int32_t x, int32_t y, int32_t z,
    uint16_t machine_id,
    const std::vector<uint8_t>& inventory_data,
    float progress,
    uint32_t energy,
    EnergyType energy_type = EnergyType::ELECTRICITY  // NEW
) = 0;
```

---

## Client: UI

### `BlockType.h` — минимальный enum, остальное runtime

```cpp
enum class BlockType : uint16_t {
    Unknown        = 0,
    CraftingTable  = 14,
    Chest          = 37,
    // Машины — runtime через MachineRegistry
};
```

Любой `uint16_t` можно проверить через `MachineRegistry::IsMachine()`.

### Моки не нужны

MachineWindow и так падает на "No mechanism" при отсутствии `mech_`:

```cpp
if (!mech_) {
    ImGui::Text("No mechanism");
    ImGui::End();
    return;
}
```

Реальные данные приходят с сервера через `OnNetworkUpdate`. Клиент не симулирует
логику машин — это работа simcore.

Регистрация окон в `BlockUIFactory` — цикл по MachineRegistry (оба CSV):

```cpp
void BlockUIFactory::LoadFromRegistry(const MachineRegistry& reg) {
    for (auto& [id, info] : reg.All()) {
        BlockType bt = static_cast<BlockType>(id);
        GetRegistry()[bt] = [](UIManager& mgr, BlockPos pos) -> IUIWindow* {
            return FindOrCreateMachine(mgr, pos, bt);
        };
    }
}
```

### `MachineWindow` — лейбл энергии

```cpp
const char* label = "EU";
if (mech_) {
    switch (mech_->GetEnergyType()) {
        case EnergyType::ELECTRICITY: label = "EU";  break;
        case EnergyType::HEAT:        label = "HU";  break;
        case EnergyType::STEAM:       label = "SU";  break;
    }
}
snprintf(energyStr, sizeof(energyStr), "%u / %u %s", energy, capacity, label);
```

### Рецепты — файлы по block_id

```
data/recipes/36.json    (heat_furnace)
data/recipes/51.json    (steam_macerator)
...
```

Внутри JSON поле `"m"` не нужно — ID уже в имени файла.

`ClientMachineRecipeDB::MachineTypeFromFilename()` — удаляется.
Замена: загрузка по `{block_id}.json`.

---

## RecipeManager — внутренние изменения

### `RecipeTypes.h`

```cpp
// Было:
Protocol::MachineType machine;

// Стало:
uint16_t machine_id;       // block_id из items.csv
```

### `RecipeManager.cpp` — парсинг рецептов

```cpp
// Было: читали "m" как MachineType enum
recipe.machine = static_cast<Protocol::MachineType>(data["m"].get<int>());

// Стало: читаем как block_id (uint16) или из имени файла
recipe.machine_id = data["m"].get<uint16_t>();
```

`findRecipeByInputs(machine_type, items)` → `findRecipeByInputs(machine_id, items)`.

---

## Что удаляется / заменяется

| Было | Стало |
|------|-------|
| `MachineType` enum (recipe.fbs) | `machine_id:uint16` |
| `BlockType` enum (simcore) с ручной синхронизацией | `MachineRegistry` + единый `uint16` |
| `ClientMachineRecipeDB::MachineTypeFromFilename()` | файл `{id}.json` |
| Per-machine классы моков (MockFurnace, MockMacerator...) | **Не нужны** — данные с сервера |
| Per-machine регистрация в BlockUIFactory | Цикл по Registry |
| `EntitySnapshot.energy` как EU | Энергия типизирована через `EnergyType` |
| `EnergyStorage` без типа | `EnergyStorage.type` + `EnergySource` tag-component |
| Один `machines.csv` | `consumers.csv` + `producers.csv` |

---

## Агенты (порядок реализации)

| # | Агент | Что делает | Зависит от |
|---|-------|------------|------------|
| A | **Data + MachineRegistry** | `consumers.csv` + `producers.csv`, `MachineRegistry.h/.cpp` (lib), парсер CSV | — |
| B | **Protocol** | core.fbs: `EnergyType` + `BlockEntityUpdate.energy_type`; recipe.fbs: `MachineType`→`uint16` | — |
| C | **Simcore** | `MachineComponent` под `machine_id`, `EnergyStorage.type`, `EnergySource` tag, `HeatIntakeComponent`, `MachineFactory`, `MachineSystem`, `EnergyDistributionSystem` | A + B |
| D | **Client** | `BlockUIFactory` из Registry, `MachineWindow` лейблы, `IMechanism.EnergyType` (no mocks) | A + B |
| E | **RecipeManager** | `machine_id` вместо MachineType, загрузка `{id}.json` | B |
| F | **(post-MVP) GeneratorSystem** | ECS-система для PRODUCER: жжёт топливо → заполняет EnergyStorage | C |

A + B независимы, можно параллельно. C — после A+B. D — после A+B. E — после B, можно параллельно с C.

---

## Open Questions

1. Энергия из `BlockEntityUpdate` сейчас `uint32` — достаточно, или нужно `int64` для GTNH-масштабов?
2. `HeatIntakeComponent` — отдельный компонент или поле в `EnergyStorage.heat_intake:std::optional<EnergyType>`?
