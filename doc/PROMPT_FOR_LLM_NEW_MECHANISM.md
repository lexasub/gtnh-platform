# Prompt для LLM: генерация данных под новый механизм

> Этот файл — инструкция для LLM (ChatGPT, Claude, Gemini и т.д.), которая **не видит код проекта**.
> Ваша задача — сгенерировать "наброски" (recipes JSON, items CSV, ECS-компоненты, enum'ы)
> для **нового механизма** так, чтобы разработчик потом вписал это в реальный C++ проект.
>
> **ВАЖНО**: Клиентские моки (Mock*) не нужны. Настоящая обработка машин
> уже реализована в SimulationCore (ECS MachineSystem — generic, не требует
> доработок под каждую машину). MachineWindow работает и без мока — показывает
> "No mechanism" и ждёт данные из сети. Если хотите, мок можно добавить
> опционально (пункт 12), но это не обязательно.

## Что нужно сделать

Придумайте **новую машину** для GTNH-подобной песочницы и сгенерируйте для неё:

1. **Рецепты** в JSON (формат проекта)
2. **Новые предметы** для items.csv (сырьё, ингредиенты, результаты)
3. **recipe.fbs MachineType enum** (новое значение)
4. **ECS-компоненты** (подсказка: ничего писать не надо — MachineComponent/RecipeProgress/InventoryContainer/EnergyStorage уже есть и переиспользуются)
5. **BlockType enum** на клиенте
6. **Регистрация в BlockUIFactory** (какие строки добавить)
7. **NEI маппинг** (ClientMachineRecipeDB — MachineTypeFromFilename)

## 1. Формат рецептов (JSON)

### Общий формат файла

Файлы лежат в `data/recipes/{machine_name}.json`. Каждый файл — JSON-объект,
где ключ — ID рецепта, значение — объект рецепта:

```json
{
  "recipe_id_1": {
    "m": 2,           // MachineType (число): 1=Furnace, 2=Assembler, 3=Crystallizer, 4=Electrolyser, 5=ChemicalReactor
    "in": [[4, 6]],   // вход: массив [item_id, count?]
    "out": [[9]],     // выход: массив [item_id, count?]
    "dur": 200        // длительность в тиках (20 тиков/сек)
  }
}
```

### Детали формата

- **`in`** — массив входов. Каждый вход: `[item_id]` (count=1) или `[item_id, count]`.
  Для крафтовых столов (m=0) — ровно 9 элементов (3×3 сетка), пустые = `[0, 0]`.
  Для машин — каждый элемент = отдельный слот инпута.
  Можно добавить `{"consume": false}` третьим элементом для "ведёрных" рецептов:
  ```json
  "in": [[16, 1, {"consume": false}]]  // вода не расходуется
  ```

- **`out`** — массив выходов. Каждый: `[item_id]` или `[item_id, count]`.

- **`dur`** — тики крафта (20 ticks = 1 секунда).

- **`eu`** (опционально) — энергозатраты в EU/tick.

- **`conditions`** (опционально) — см. раздел 6.

### Реальные примеры из проекта

**macerator.json** (MachineType=1, один вход → один выход):
```json
{
  "minecraft:macerator_iron_ore": { "m": 1, "in": [[3]], "out": [[9, 2]], "dur": 400 },
  "minecraft:macerator_gold_ore": { "m": 1, "in": [[5]], "out": [[9, 2]], "dur": 400 },
  "minecraft:macerator_cobblestone": { "m": 1, "in": [[7]], "out": [[11]], "dur": 100 }
}
```

**assembler.json** (MachineType=2, два входа → один выход):
```json
{
  "gtnh:assembler_dust": { "m": 2, "in": [[4, 6]], "out": [[9]], "dur": 200 },
  "gtnh:assembler_rod":  { "m": 2, "in": [[4, 6]], "out": [[10]], "dur": 200 },
  "gtnh:assembler_glass": { "m": 2, "in": [[11], [12]], "out": [[12]], "dur": 200 }
}
```

**furnace.json** (MachineType=1, с опциональным eu):
```json
{
  "minecraft:furnace_beef": { "m": 1, "in": [[1]], "out": [[2]], "dur": 200 },
  "minecraft:furnace_iron_ore": { "m": 1, "in": [[3]], "out": [[4]], "dur": 200, "eu": 0.7 }
}
```

**crafting_table.json** (MachineType=0, 9 слотов 3×3):
```json
{
  "minecraft:stick": {
    "m": 0,
    "in": [[13, 1], [0, 0], [0, 0], [13, 1], [0, 0], [0, 0], [0, 0], [0, 0], [0, 0]],
    "out": [[32, 4]],
    "dur": 100
  }
}
```

## 2. Формат items.csv

Файл: `data/registry/items.csv` (CSV с заголовком и 47 строчками на данный момент).

```
id,name,stack_size,meta
0,minecraft:air,0,0
1,minecraft:beef,64,0
2,minecraft:cooked_beef,64,0
3,minecraft:iron_ore,64,0
...и т.д.
45,minecraft:brick_block,64,0
```

**Правила:**
- `id` — uint16, последовательный. Предыдущий ID = 45. Новые предметы **начинаются с 46**.
- `name` — `{domain}:{name}`. Для GTNH-специфичных предметов используйте `gtnh:`.
  Для ванильных — `minecraft:`.
- `stack_size` — 64 (обычные), 16 (вёдра/особые), 1 (инструменты).
- `meta` — всегда 0 для базовых предметов.
- **Не переопределяйте существующие ID 0-45.**

### Текущие предметы (для справки)

Наиболее релевантные для рецептов:
- 1:beef, 2:cooked_beef, 3:iron_ore, 4:iron_ingot, 5:gold_ore, 6:gold_ingot
- 7:cobblestone, 8:stone, 9:dust, 10:rod, 11:sand, 12:glass
- 13:oak_planks, 14:crafting_table, 16:water_bucket(16), 17:hydrogen_bucket(16)
- 18:sulfuric_acid_bucket(16), 19:quartz_ore, 20:quartz
- 21:obsidian, 22:crystal, 23:electrum_ore, 24:electrum_ingot
- 25:tin_ore, 26:tin_ingot, 27:uranium_ore, 28:uranium_ingot
- 29:alloy, 31:oxygen, 32:stick, 36:furnace_block, 37:chest
- 44:coal, 45:brick_block

## 3. Типы окон (IUIWindow hierarchy)

Клиент использует ImGui. Иерархия окон:

```
IUIWindow
 ├── InventoryWindow          (инвентарь + хотбар)
 ├── CreativeMenu             (креатив)
 └── BlockAttachedWindow      (окна, привязанные к блоку в мире)
      ├── CraftingWindow      (верстак: 3×3 сетка + результат)
      ├── ChestWindow         (сундук: 27 слотов)
      └── MachineWindow       (машина: инпуты → прогресс → аутпуты + энергия)
```

**MachineWindow** — универсальное окно для всех машин. Оно **не надо переписывать**:
- Читает слоты из `IMechanism` интерфейса (сколько инпутов, сколько аутпутов)
- Рендерит прогресс-бар и энергобар
- Принимает `BlockEntityUpdate` из сети

### Принцип работы MachineWindow

```cpp
// Окно создаётся по Factory, получает IMechanism и каждый кадр:
auto& inSlots  = mech_->GetInputSlots();   // входные слоты
auto& outSlots = mech_->GetOutputSlots();  // выходные слоты
float progress = mech_->GetProgress();     // 0.0-1.0
auto [energy, maxEnergy] = mech_->GetEnergy();

// Рендерит: [инпуты...] [ProgressBar] [аутпуты...] [EnergyBar] [инвентарь игрока]
```

Никакой новой имплементации окна для новой машины **не требуется**.

## 4. IMechanism (клиентская заглушка — НЕ ТРЕБУЕТСЯ)

**Mock* классы НЕ НУЖНЫ для добавления новой машины.** Они существуют только
как временная заглушка для UI-превью при отсутствии сети.

### Почему они не нужны

Настоящая обработка машин — в `SimulationCore::MachineSystem::tick()`.
Она **generic**: работает по `MachineType` enum, не требует специализации:

```cpp
auto view = reg_.view<MachineComponent, RecipeProgress, InventoryContainer, EnergyStorage>();
for (auto ent : view) {
    auto& machine = view.get<MachineComponent>(ent);
    auto* recipe = recipes_->findRecipeByInputs(
        static_cast<Protocol::MachineType>(machine.machine_type), inputItems);
    // ... generic craft, generic publishBlockEntityUpdate ...
}
```

Никакого per-machine кода. Добавили MachineType в recipe.fbs и рецепты в JSON —
сервер уже умеет их обрабатывать.

### Что будет в UI без мока

`BlockUIFactory::FindOrCreateMachine()` создаёт `MachineWindow` и проверяет:
```cpp
auto* win = FindOrCreate<MachineWindow>(mgr, pos, type);
if (!win->GetMechanism()) {
    auto& reg = GetMechanismRegistry();
    if (auto it = reg.find(type); it != reg.end())  // если нет мока — пропускает
        win->SetMechanism(it->second());
}
return win;
```

Если в `MechanismRegistry` нет записи для этой машины — `win->GetMechanism()` == nullptr.
MachineWindow рендерит "No mechanism" (см. `MachineWindow::Render`):
```cpp
if (!mech_) {
    ImGui::Text("No mechanism");
    ImGui::End();
    return;
}
```

**Окно не падает.** Показывает сообщение и ждёт `BlockEntityUpdate` из сети.
Как только SimulationCore начнёт публиковать апдейты — `OnNetworkUpdate()`
заполнит `pendingUpdate_` и UI покажет прогресс/энергию.

### Когда мок всё-таки нужен (опционально)

Если вы хотите видеть анимированный UI при локальном тесте клиента без
поднятого сервера — см. **раздел 12** (опциональные моки). В остальных
случаях — не нужно.

## 5. SimulationCore (ECS-система)

На сервере машины управляются через ECS (EnTT) в `simulation_core`:

### Компоненты машины

ECS-компонент для позиции и типа машины:
```cpp
struct MachineComponent {
    uint32_t x, y, z;
    uint16_t machine_type;  // Protocol::MachineType
};
```

Остальные компоненты:
```cpp
struct RecipeProgress {
    std::string recipe_id;
    int32_t remaining_ticks = 0;
    bool is_processing = false;
    bool needs_output = false;
};

struct InventoryContainer {
    std::vector<InventorySlot> slots;  // item_id, count, meta
};

struct EnergyStorage {
    int32_t capacity;
    int32_t current;
    int32_t max_input;
    int32_t max_output;
    int32_t tier;
};
```

### MachineSystem::tick()

Вызывается 20 раз/сек. Для каждой сущности с компонентами `MachineComponent + RecipeProgress + InventoryContainer + EnergyStorage`:

1. Если не в процессе и нет active recipe — ищет рецепт через `RecipeManager::findRecipeByInputs(machineType, inputs)`
2. Если рецепт найден и `evaluateConditions` успешен — запускает: `progress.recipe_id = recipe.id; remaining_ticks = duration`
3. Каждый тик: проверяет энергию (`energy.current >= recipe.energy_cost`), вычитает, уменьшает `remaining_ticks`
4. Когда `remaining_ticks == 0` — крафтит через `recipe.craft(inputItems)`, обновляет слоты
5. Публикует `BlockEntityUpdate` с прогрессом и энергией

**Дополнительные ECS-компоненты** (уже есть в проекте для условий):

```cpp
struct TemperatureComponent { float temperature; };
struct PurityComponent { float purity; };
struct BiomeComponent { uint16_t biome_id; };
struct NetworkConnectionComponent { std::vector<uint32_t> network_ids; };
struct MachineTagComponent { std::vector<SpecialCondition> tags; };
```

## 6. RecipeConditions (опциональные условия для рецептов)

Рецепты могут иметь условия в JSON:
```json
{
  "gtnh:advanced_recipe": {
    "m": 5,
    "in": [[18]],
    "out": [[18]],
    "dur": 200,
    "conditions": {
      "environment": { "temperature": {"min": 100, "max": 500}, "purity": 0.8, "biomes": [1, 2] },
      "machine": { "energy_min": 30, "energy_max": 120, "network_id": 42, "facing": 2 },
      "special": [{"key": 1, "value_type": 0, "int_value": 7}]
    }
  }
}
```

## 7. RecipeTypes (как устроены рецепты внутри)

```cpp
struct Recipe {
    std::string id;
    std::vector<InputItem> inputs;    // item_id, count, consume, replace_item
    std::vector<OutputItem> outputs;  // item_id, count, metadata, display_name, nbt, lore
    Protocol::MachineType machine;    // enum {NONE, FURNACE, ASSEMBLER=2, CRYSTALLIZER=3, ELECTROLYSER=4, CHEMICAL_REACTOR=5}
    uint32_t duration;                // ticks
    float energy_cost;                // EU/t
    RecipeConditions conditions;      // environment, machine, special
};
```

## 8. FlatBuffers MachineType enum

```fbs
enum MachineType : uint8 {
  NONE = 0,
  FURNACE = 1,
  ASSEMBLER = 2,
  CRYSTALLIZER = 3,
  ELECTROLYSER = 4,
  CHEMICAL_REACTOR = 5,
}
```

Для новой машины надо добавить **следующее значение**: `MACHINENAME = 6`.

## 9. BlockUIFactory — регистрация

На клиенте все блоки с UI регистрируются в `BlockUIFactory`:

```cpp
// Регистрация окна (открывается при клике на блок)
r[BlockType::NewMachine] = [](UIManager& mgr, BlockPos pos) -> IUIWindow* {
    return FindOrCreateMachine(mgr, pos, BlockType::NewMachine);
};

// Регистрация мока (клиентская заглушка)
r[BlockType::NewMachine] = []() { return std::make_unique<MockNewMachine>(); };
```

## 10. NEI-отображение

`NeiPanel` автоматически показывает рецепты для машины, когда MachineWindow открыт.
Он берёт рецепты из `ClientMachineRecipeDB`, который читает `data/recipes/*.json`.

**Проблема**: `ClientMachineRecipeDB::MachineTypeFromFilename()` надо дополнить —
сейчас он знает только `furnace` → `BlockType::Furnace` и `crafting_table` → `BlockType::CraftingTable`.

```cpp
BlockType MachineTypeFromFilename(const std::string& name) {
    // вырезаем .json
    if (fname == "furnace")           return BlockType::Furnace;
    if (fname == "crafting_table")    return BlockType::CraftingTable;
    // НОВЫЕ МАШИНЫ:
    if (fname == "macerator")         return BlockType::Macerator;
    if (fname == "compressor")        return BlockType::Compressor;
    if (fname == "your_machine")      return BlockType::YourMachine;
    return BlockType::Unknown;
}
```

## 11. BlockType enum (клиент)

На клиенте `enum class BlockType : uint16_t`:

```cpp
enum class BlockType : uint16_t {
    Unknown        = 0,
    // ... существующие блоки ...
    CraftingTable  = 14,   // ID из items.csv
    Furnace        = 36,
    Chest          = 37,
    // НОВЫЕ: ID берутся из items.csv (46+)
    Macerator      = 46,
    Compressor     = 47,
    YourMachine    = 48,
};
```

Конвертация `uint16_t ↔ BlockType` и `ToString()` в `BlockType.h` тоже требует обновления.

## Сводная таблица что генерировать

| Артефакт | Куда | Формат | Обязательно? |
|----------|------|--------|:---:|
| Рецепты | `data/recipes/{name}.json` | JSON (примеры выше) | ✅ |
| Предметы | `data/registry/items.csv` | CSV (id начинать с 46) | ✅ |
| MachineType (FBS) | `protocol/recipe.fbs` | новое значение enum | ✅ |
| BlockType | `Common/BlockType.h` | новое значение enum | ✅ |
| BlockUIFactory | `UI/BlockUIFactory.cpp` | регистрация окна | ✅ |
| MachineTypeFromFilename | `Crafting/ClientMachineRecipeDB.cpp` | новый case | ✅ |
| BlockEntityUpdate | Data: 8 bytes (uint32 energy + float progress) | бинарный payload | ✅ |
| NEI отображение | автоматически через ClientMachineRecipeDB | — | ✅ |
| MockMechanism.h (опц.) | `ServerLogic/Mock{Name}.h` | C++ класс | ❌ опционально |
| ECS компоненты (опц.) | SimulationCore | уже есть, не трогать | ❌ |

---

## Чеклист сгенерированного

### Обязательно

- [ ] `data/recipes/{machinename}.json` — 3-5 рецептов с входами/выходами.
      Ключевой момент: число слотов `in` должно соответствовать физическому
      числу слотов у машины (Macerator/Furnace=1, Assembler=2, Electrolyser=1 in → 2 out).
- [ ] Номера предметов для входов/выходов (с проверкой conflicts с items.csv)
- [ ] Несколько новых записей для `items.csv` (46+)
- [ ] Значение `enum MachineType` для `recipe.fbs` (следующий номер: 6, 7, ...)
- [ ] Значение `BlockType` enum для `Common/BlockType.h` (ID = items.csv ID)
- [ ] Маппинг в `BlockUIFactory::GetRegistry()` — `r[BlockType::NewMachine] = ...`
- [ ] Маппинг в `ClientMachineRecipeDB::MachineTypeFromFilename()` — новый `if (fname == ...)`

### Опционально (только для UI-превью без сервера)

- [ ] Клиентский `Mock{Name}.h` с правильным числом слотов, cookTime и energyCost
- [ ] Маппинг в `BlockUIFactory::GetMechanismRegistry()` — регистрация мока
