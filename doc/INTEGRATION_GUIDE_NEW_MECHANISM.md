# Интеграция нового механизма в проект

> Этот файл — инструкция для разработчика, который берёт "наброски" из
> `PROMPT_FOR_LLM_NEW_MECHANISM.md` и **вписывает их в реальный код проекта**.
>
> Предполагается что LLM уже сгенерировала:
> - JSON рецептов (`data/recipes/{name}.json`)
> - CSV записи предметов (`data/registry/items.csv`)
> - Mock-класс (`src/services/game_client/ServerLogic/Mock{Name}.h`)
> - Имена/значения для enum'ов

## Оглавление

1. [Порядок действий](#1-порядок-действий)
2. [Рецепты JSON](#2-рецепты-json)
3. [Реестр предметов items.csv](#3-реестр-предметов-itemscsv)
4. [MachineType (FlatBuffers)](#4-machinetype-flatbuffers)
5. [BlockType enum (клиент)](#5-blocktype-enum-клиент)
6. [Mock-класс (клиент)](#6-mock-класс-клиент)
7. [BlockUIFactory — регистрация](#7-blockuifactory--регистрация)
8. [ClientMachineRecipeDB — NEI](#8-clientmachinerecipedb--nei)
9. [SimulationCore — MachineType mapping](#9-simulationcore--machinetype-mapping)
10. [RecipeManager — загрузка рецептов](#10-recipemanager--загрузка-рецептов)
11. [Чеклист верификации](#11-чеклист-верификации)
12. [Советы по отладке](#12-советы-по-отладке)

---

## 1. Порядок действий

Оптимальный порядок (чтобы не было билд-брейков):

```
1. items.csv            → новые ID предметов
2. recipe.fbs           → новый MachineType enum
3. BlockType.h          → новый BlockType enum
4. data/recipes/*.json  → рецепты
5. Mock{Name}.h         → клиентская заглушка
6. BlockUIFactory.cpp   → регистрация окна + мока
7. ClientMachineRecipeDB.cpp → MachineTypeFromFilename
8. RecipeManager → загрузка recipe JSON (если парсер не знает новой машины)
9. SimulationCore → если нужна новая MachineType в map
```

---

## 2. Рецепты JSON

**Файл**: `data/recipes/{machinename}.json`

Просто поместите сгенерированный JSON в эту директорию. Никакой регистрации не нужно —
`RecipeManager::loadRecipesFromDirectory()` и `ClientMachineRecipeDB::LoadFromDirectory()`
читают все `.json` файлы рекурсивно.

**Валидация:**
- `"m"` — должно совпадать с новым значением MachineType (6, 7, и т.д.)
- `"in"` / `"out"` — `item_id` должны существовать в `items.csv`
- Если используете `{"consume": false}` — входной предмет будет скопирован, а не потрачен
- Для `crafting_table` (m=0): 9 элементов в `in` (3×3)

---

## 3. Реестр предметов items.csv

**Файл**: `data/registry/items.csv`

### Добавление строк

```diff
 45,minecraft:brick_block,64,0
+46,gtnh:macerator,64,0
+47,gtnh:compressor,64,0
+48,gtnh:crushed_iron,64,0
+49,gtnh:crushed_gold,64,0
+50,gtnh:industrial_grinder,64,0
```

### Именование

- **Блок машины** (можно поставить в мир): `gtnh:{machinename}` — ID должен совпадать с BlockType.
  Пример: машина Macerator → `gtnh:macerator` → ID=46 → `BlockType::Macerator = 46`.
- **Ингредиенты**: `gtnh:{material}` или `minecraft:{vanilla_name}`.
- **Инструменты**: `gtnh:{item_name}`.

### Правила

- **ID**: просто следующий свободный номер после 45.
- **stack_size**: 64 для блоков/ресурсов, 16 для вёдер/контейнеров, 1 для инструментов.
- **Не удаляйте** существующие строки.
- **Не меняйте ID** существующих предметов.

---

## 4. MachineType (FlatBuffers)

### Файл: `src/protocol/recipe.fbs`

Добавьте новое значение в enum:

```diff
 enum MachineType : uint8 {
   NONE = 0,
   FURNACE = 1,
   ASSEMBLER = 2,
   CRYSTALLIZER = 3,
   ELECTROLYSER = 4,
   CHEMICAL_REACTOR = 5,
+  MACERATOR = 6,
+  COMPRESSOR = 7,
 }
```

После правки надо **перегенерировать FlatBuffers**:

```bash
flatc --cpp --go -o src/protocol/generated src/protocol/*.fbs
```

Или, если есть скрипт сборки:
```bash
cd build && cmake .. && make generated
```

`flatc` сгенерирует `recipe_generated.h`, который используют и клиент, и сервер.

---

## 5. BlockType enum (клиент)

### Файл: `src/services/game_client/Common/BlockType.h`

```diff
 enum class BlockType : uint16_t {
     Unknown        = 0,
     CraftingTable  = 14,
     Furnace        = 36,
     Chest          = 37,
+    Macerator      = 46,   // ID из items.csv
+    Compressor     = 47,
 };

 // Обновить счётчик:
-inline constexpr size_t kBlockTypeCount = 3;
+inline constexpr size_t kBlockTypeCount = 5;
```

### ToBlockType() — конвертация из uint16_t

```diff
 constexpr BlockType ToBlockType(uint16_t v) noexcept {
     if (v == static_cast<uint16_t>(BlockType::CraftingTable) ||
         v == static_cast<uint16_t>(BlockType::Furnace) ||
-        v == static_cast<uint16_t>(BlockType::Chest))
+        v == static_cast<uint16_t>(BlockType::Chest) ||
+        v == static_cast<uint16_t>(BlockType::Macerator) ||
+        v == static_cast<uint16_t>(BlockType::Compressor))
         return static_cast<BlockType>(v);
     return BlockType::Unknown;
 }
```

### ToString() — отображение в UI

```diff
 constexpr std::string_view ToString(BlockType t) noexcept {
     switch (t) {
         case BlockType::CraftingTable: return "Crafting Table";
         case BlockType::Furnace:       return "Furnace";
         case BlockType::Chest:         return "Chest";
+        case BlockType::Macerator:     return "Macerator";
+        case BlockType::Compressor:    return "Compressor";
         default:                       return "Unknown";
     }
 }
```

---

## 6. Mock-класс (клиент)

### Файл: `src/services/game_client/ServerLogic/Mock{Name}.h`

**Создайте новый файл** на основе сгенерированного LLM кода.

### Шаблон для однослотовой машины (Furnace-like)

```cpp
#pragma once

#include "Common/BlockType.h"
#include "ServerLogic/IMechanism.h"

class MockMacerator : public IMechanism {
public:
    MockMacerator() : energy_(maxEnergy_) {}

    BlockType GetMachineType() const override { return BlockType::Macerator; }
    std::vector<ItemStack>& GetInputSlots() override { return input_; }
    std::vector<ItemStack>& GetOutputSlots() override { return output_; }
    float GetProgress() const override { return progress_; }
    std::pair<uint32_t, uint32_t> GetEnergy() const override { return {energy_, maxEnergy_}; }

    void Tick(float dt) override {
        if (energy_ == 0) return;
        if (input_[0].count > 0) {
            bool canOutput = output_[0].count == 0
                || (output_[0].item_id == input_[0].item_id && output_[0].count < 64);
            if (canOutput) {
                progress_ += dt / cookTime_;
                if (progress_ > 1.0f) progress_ = 1.0f;
                energy_ = std::max((uint32_t)0, energy_ - energyCost_);
                if (progress_ >= 1.0f) {
                    output_[0] = input_[0];
                    input_[0] = {};
                    progress_ = 0.0f;
                }
            }
        }
    }

private:
    std::vector<ItemStack> input_{1};    // число входных слотов
    std::vector<ItemStack> output_{1};   // число выходных слотов
    float progress_ = 0.0f;
    uint32_t energy_ = 0;
    uint32_t maxEnergy_ = 300;           // энергоёмкость
    float cookTime_ = 8.0f;              // время крафта (сек)
    uint32_t energyCost_ = 2;            // EU/tick
};
```

### Вариации

| Тип | input_ | output_ | cookTime_ | maxEnergy_ | energyCost_ |
|-----|--------|---------|-----------|------------|-------------|
| Furnace | 1 | 1 | 5.0 | 300 | 1 |
| Macerator | 1 | 1 | 8.0 | 300 | 2 |
| Compressor | 1 | 1 | 10.0 | 300 | 3 |
| Assembler | 2 | 1 | 6.0 | 400 | 4 |
| Electrolyser | 1 | 2 | 7.0 | 500 | 5 |
| ChemicalReactor | 1-2 | 1 | 8.0 | 600 | 6 |
| Centrifuge | 1 | 3+ | 10.0 | 400 | 3 |
| Mixer | 2-4 | 1 | 12.0 | 500 | 5 |

---

## 7. BlockUIFactory — регистрация

### Файл: `src/services/game_client/UI/BlockUIFactory.cpp`

### Добавление окна

```diff
 IUIWindow* BlockUIFactory::FindOrCreateMachine(...);
 
 BlockUIFactory::Registry& BlockUIFactory::GetRegistry() {
     static Registry reg = []() {
         Registry r;
         r[BlockType::CraftingTable] = ...;
         r[BlockType::Furnace] = ...;
         r[BlockType::Chest] = ...;
+        // --- НОВЫЕ МАШИНЫ ---
+        r[BlockType::Macerator] = [](UIManager& mgr, BlockPos pos) -> IUIWindow* {
+            return FindOrCreateMachine(mgr, pos, BlockType::Macerator);
+        };
+        r[BlockType::Compressor] = [](UIManager& mgr, BlockPos pos) -> IUIWindow* {
+            return FindOrCreateMachine(mgr, pos, BlockType::Compressor);
+        };
         return r;
     }();
     return reg;
 }
```

### Добавление мока

```diff
+#include "ServerLogic/MockMacerator.h"
+#include "ServerLogic/MockCompressor.h"
+
 BlockUIFactory::MechanismRegistry& BlockUIFactory::GetMechanismRegistry() {
     static MechanismRegistry reg = []() {
         MechanismRegistry r;
         r[BlockType::Furnace] = []() { return std::make_unique<MockFurnace>(); };
+        r[BlockType::Macerator] = []() { return std::make_unique<MockMacerator>(); };
+        r[BlockType::Compressor] = []() { return std::make_unique<MockCompressor>(); };
         return r;
     }();
     return reg;
 }
```

`FindOrCreateMachine()` уже автоматически:
1. Создаёт `MachineWindow` с нужным `BlockType`
2. Если у окна нет `IMechanism` — ищет фабрику в `MechanismRegistry`
3. Вызывает `win->SetMechanism(factory())`

---

## 8. ClientMachineRecipeDB — NEI

### Файл: `src/services/game_client/Crafting/ClientMachineRecipeDB.cpp`

NEI-панель автоматически показывает рецепты для открытой машины.
Надо дополнить маппинг имени файла → BlockType:

```diff
 BlockType MachineTypeFromFilename(const std::string& name) {
     std::string fname = name;
     if (fname.size() >= 5 && fname.substr(fname.size() - 5) == ".json")
         fname.resize(fname.size() - 5);
 
     if (fname == "furnace")           return BlockType::Furnace;
     if (fname == "crafting_table")    return BlockType::CraftingTable;
+    if (fname == "macerator")         return BlockType::Macerator;
+    if (fname == "compressor")        return BlockType::Compressor;
 
     return BlockType::Unknown;
 }
```

> **Важно**: Имя файла `data/recipes/{name}.json` должно совпадать с названием здесь.
> Если файл называется `macerator.json` — `fname == "macerator"`.

Если `MachineTypeFromFilename` возвращает `Unknown`, рецепты загружены **не будут**,
и NEI покажет "No recipes found". Проверьте лог:
```
MachineRecipeDB: unknown machine type for file macerator.json
```

---

## 9. SimulationCore — MachineType mapping

### Когда нужно

Если SimulationCore использует отдельный map для маппинга `MachineType` → что-то (парсеры, логика).

### Где смотреть

Проверьте `src/services/simulation_core/ECS/Systems/MachineSystem.cpp`:
```cpp
auto* recipe = recipes_->findRecipeByInputs(
    static_cast<Protocol::MachineType>(machine.machine_type), inputItems);
```

`findRecipeByInputs` использует `MachineType` напрямую из FlatBuffers enum.
Если в `recipe.fbs` добавили новое значение — оно уже доступно как `Protocol::MachineType::MACERATOR`.

### Ничего не надо делать, если:

- MachineSystem уже использует `Protocol::MachineType` напрямую (generic)
- RecipeManager::findRecipeByInputs работает по enum'у
- Новые рецепты JSON с `"m": 6` будут найдены

### Надо сделать, если:

- Есть switch/case или map по `MachineType` для специфической логики
- Нужна особая обработка для новой машины (например, multi-block validation)

Проверьте grep'ом:
```bash
grep -rn "MachineType" src/services/simulation_core/ --include="*.cpp" --include="*.h"
```

---

## 10. RecipeManager — загрузка рецептов

### Как это работает

`RecipeManager::loadRecipesFromDirectory("data/recipes/")`:
1. Открывает директорию
2. Для каждого `.json` файла вызывает `parseRecipeFile(nlohmann::json)`
3. Для каждого объекта внутри JSON вызывает `parseCompactRecipe(recipeId, data)`
4. `parseCompactRecipe` читает `"m"` как `MachineType` и вызывает `stringToMachineType` только для legacy-формата

**Никакой регистрации не нужно**. Новый `macerator.json` с `"m": 6` (Macerator) будет
автоматически загружен и привязан к `MachineType::MACERATOR`.

### Проверка

Запустите сервер и проверьте лог:
```bash
./build/simcored
# Ищите: "Loaded N recipes" или "Loaded recipe_id"
```

Или добавьте временную печать:
```cpp
spdlog::info("RecipeManager: loaded {} recipes for machine type {}",
    count, Protocol::EnumNameMachineType(machineType));
```

---

## 11. Чеклист верификации

После всех изменений, проверьте:

### Клиент (GameClient)

- [ ] `cookTime_` и `energyCost_` установлены реалистично (Furnace ~5s/1EU, Macerator ~8s/2EU)
- [ ] Билд клиента проходит: `cmake --build build --target client`
- [ ] `lsp_diagnostics` чист на изменённых файлах
- [ ] `BlockType::ToString` показывает человеческое имя
- [ ] При клике на блок машины в мире открывается окно
- [ ] Окно показывает инпут-слоты, прогресс-бар и энергобар
- [ ] При нажатии по машине Tab открывается NEI с рецептами
- [ ] Мок тикает (прогресс растёт, энергия убывает)

### Сервер (SimulationCore)

- [ ] Рецепты загружаются (проверить лог)
- [ ] `findRecipeByInputs` находит рецепты для нового MachineType
- [ ] `evaluateConditions` работает (если есть условия)
- [ ] `publishBlockEntityUpdate` отправляет корректные progress/energy
- [ ] Билд сервера проходит

### Протокол

- [ ] `flatc` регенерация прошла без ошибок
- [ ] `recipe_generated.h` содержит новый MachineType
- [ ] Все сервисы, импортящие протокол, пересобраны

### Интеграция

- [ ] `items.csv` ID не конфликтуют (каждое уникально)
- [ ] BlockType enum значение совпадает с ID машины в items.csv
- [ ] MachineType enum в recipe.fbs совпадает с `"m"` в JSON рецептов
- [ ] `MachineTypeFromFilename` знает новый файл рецептов
- [ ] `ToBlockType()` включает новое значение
- [ ] `kBlockTypeCount` обновлён

---

## 12. Советы по отладке

### Окно не открывается при клике на блок

Сценарий:
1. Клиент получает `BlockType` из мира
2. Вызывает `BlockUIFactory::Create(blockId, pos, mgr)`
3. Если `blockId` нет в `Registry` — возвращает `nullptr`

Проверьте:
- `BlockType::Macerator` определён в `BlockType.h`
- `ToBlockType(uint16_t)` возвращает `BlockType::Macerator` (не `Unknown`)
- `r[BlockType::Macerator]` зарегистрирован в `BlockUIFactory::GetRegistry()`

### NEI не показывает рецепты

Проверьте:
- `ClientMachineRecipeDB::LoadAll()` вызывается при старте
- Файл `data/recipes/macerator.json` читается (лог `"loaded N recipes from macerator.json"`)
- `MachineTypeFromFilename("macerator.json")` возвращает `BlockType::Macerator`
- В `NeiPanel::RenderMachineRecipes` вызывается `MachineRecipes::GetRecipes(machineType)`

### Прогресс-бар не движется

Проверьте:
- `MachineWindow::Tick(dt)` вызывается каждый кадр из `GameClient::Update`
- `mech_->Tick(dt)` не падает
- `MockMacerator` создан (не nullptr)
- `GetProgress()` возвращает значение > 0

### Сервер не находит рецепты

Проверьте:
- `data/recipes/macerator.json` существует
- `RecipeManager::loadRecipesFromDirectory("data/recipes/")` вызывается
- `"m": 6` в JSON совпадает с `Protocol::MachineType::MACERATOR`
- `recipes_->findRecipeByInputs(Protocol::MachineType::MACERATOR, inputItems)` вызывается
- Input items совпадают по `item_id` с рецептом
- `evaluateConditions` не блокирует рецепт

### Quick debug: временная печать

Добавьте в MachineSystem::tick():
```cpp
SPDLOG_INFO("Machine {} recipe={} progress={}% energy={}/{}",
    ent, progress.recipe_id,
    (recipe ? (100 * (recipe->duration - progress.remaining_ticks) / recipe->duration) : 0),
    energy.current, energy.capacity);
```
