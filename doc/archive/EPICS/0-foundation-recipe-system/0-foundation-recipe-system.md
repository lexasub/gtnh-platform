# Систе́ма реце́птов (Recipe System)

**Epic name:** Система рецептов (Recipe System)  
**Layer:** Layer 0 → Layer 2  
**Status:** Draft → Updated (uint16 IDs, Item Registry, compact JSON, overrides)

## Affected Services

| Service | Layer | Role |
|---------|-------|------|
| **RecipeManager** ⬅️ **NEW** | L0 | Primary — in-memory recipe storage, CheckRecipe/Craft/EvaluateConditions RPCs |
| **Gateway** | L0 | Relay — forwards RPCs |
| **MessageRouter** | L0 | Transport — dispatches all recipe RPCs |
| **ItemRegistry** ⬅️ **NEW** | L0 | SQLite DB — item_id → name/stack_size mapping, read-only для всех сервисов |
| **SimulationCore** | L1 | Consumer — queries recipes during 20 Hz machine tick |
| **GameClient** | L1 | Consumer — recipe display in GUI, recipe book |

> **Architecture rule**: RecipeManager is independently accessible via MessageRouter. SimulationCore is one consumer — any service (including Gateway/Client) can call it directly.

---

## Обзор

Система рецептов определяет входные предметы и выходные предметы для различных машин и процессов. Рецепты могут включать условия: жидкие компоненты, энергопотребление, температуру, длительность работы и специальные требования (чистота, биом и др.).

### Ключевые решения

- **ID предметов — uint16**. На wire (FlatBuffers) и в hot-path (C++). Компактно, zero-copy, O(1) сравнение.
- **JSON рецептов — компактный формат**. Ключ объекта = ID рецепта. Числовые ID, короткие имена полей.
- **Item Registry — SQLite**. Единый source of truth: `data/registry/items.db`. Read-only, ленивая загрузка.
- **Output overrides** — display_name, NBT, color для кастомных выходных предметов.
- **Input overrides** — consume flag для ведёр/контейнеров.

---

## Item Registry

Единый реестр предметов — SQLite база `data/registry/items.db`, read-only для всех сервисов.

### Схема

```sql
CREATE TABLE items (
    id          INTEGER PRIMARY KEY,  -- uint16 (0–65535)
    name        TEXT NOT NULL UNIQUE,  -- "minecraft:iron_ingot"
    stack_size  INTEGER DEFAULT 64,
    meta        INTEGER DEFAULT 0
);
CREATE INDEX idx_items_name ON items(name);
```

### Формат на диске

```csv
id,name,stack_size
1,air,0
2,stone,64
3,dirt,64
42,iron_ingot,64
78,raw_beef,64
128,cooked_beef,64
1024,gtnh:circuit_basic,64
```

CSV → конвертируется в SQLite при сборке (`sqlite3 items.db < items.csv`). CSV — человекочитаемый source of truth под гитом.

### Кто и когда грузит

| Сервис | Зачем | Когда |
|--------|-------|-------|
| RecipeManager | парсинг JSON рецептов (uint16, использует ItemRegistry для name↔ID) | при старте |
| SimulationCore | матчинг рецептов по uint16, CraftRequestHandler | при старте |
| GameClient | текстуры, имена предметов в JEI/NEI, тултипы | lazy — при открытии GUI |
| MetaDB | сохранение предметов в инвентаре (uint16) | — |
| Gateway/WG | не нужен | — |

**RecipeManager и SimulationCore не трогают registry вообще.** Чистые uint16 на всём hot-path.

---

## Формат рецепта (компактный, числовые ID)

```json
{
  "furnace_beef": {
    "m": 1,
    "in": [[78]],
    "out": [[128]],
    "dur": 200
  },
  "furnace_iron": {
    "m": 1,
    "in": [[42]],
    "out": [[512]],
    "dur": 200,
    "eu": 0.5
  },
  "furnace_gold": {
    "m": 1,
    "in": [[82]],
    "out": [[514]],
    "dur": 200,
    "eu": 0.7
  },
  "assembler_circuit": {
    "m": 2,
    "in": [[1024, 3], [78, 2]],
    "out": [[2048]],
    "dur": 600,
    "eu": 30
  }
}
```

### Поля

| Поле | Описание |
|------|----------|
| `m` | MachineType (uint8) |
| `in` | [[item_id, count?], ...] — входные предметы. count по умолчанию 1 |
| `out` | [[item_id, count?, override?], ...] — выходные предметы |
| `dur` | Длительность в тиках (default 200) |
| `eu` | EU/tick (0 = не требует) |
| `xp` | Опыт (для печей, опционально) |
| `temp`, `liquid`, `purity`, `biome` | Layer 2 условия (опционально) |

### MachineType enum

```json
{
  "NONE": 0,
  "FURNACE": 1,
  "ASSEMBLER": 2,
  "CRYSTALLIZER": 3,
  "ELECTROLYSER": 4,
  "CHEMICAL_REACTOR": 5
}
```

### ItemStack на wire (FlatBuffers)

Уже зафиксировано в `core.fbs`:

```flatbuffers
struct ItemStack {
  item_id:uint16;
  count:uint8;
  meta:uint16;
}
```

Имя предмета **не передаётся** по сети. Только uint16. Display-информация — из ItemRegistry по запросу клиента.

---

## Output overrides

Название, NBT и цвет выходного предмета по умолчанию берутся из ItemRegistry (по item_id). Через оверрайд в рецепте можно переопределить:

```json
{
  "furnace_cinnamon_bun": {
    "m": 1,
    "in": [[78], [340]],
    "out": [[128, 1, {
      "display_name": "Свежая Булочка с Корицей",
      "nbt": {"bake_level": 3, "fresh": true},
      "lore": ["Испечено с любовью"]
    }]],
    "dur": 400
  },
  "assembler_cable_red": {
    "m": 2,
    "in": [[42, 2], [35, 1]],
    "out": [[2049, 4, {
      "color": "#FF0000",
      "nbt": {"insulation": 1}
    }]],
    "dur": 100,
    "eu": 10
  }
}
```

### Поля override

| Поле | Тип | Описание |
|------|-----|----------|
| `display_name` | `string` | Кастомное имя предмета (для JEI/тултипа) |
| `nbt` | `object` | Произвольные NBT-данные (инструменты, upgrade count, зачарования) |
| `color` | `string` | HEX-цвет (#RRGGBB) для окрашиваемых предметов |
| `lore` | `string[]` | Дополнительные строки в тултипе |
| `unlocalized_name` | `string` | Ключ для локализации (если нужен отдельный от предмета по умолчанию) |

Если оверрайда нет — название и прочее резолвятся из ItemRegistry по `item_id` первого элемента в `out`.

---

## Input overrides

### consume flag

Некоторые предметы не должны расходоваться полностью — ведро лавы возвращается пустым ведром:

```json
{
  "furnace_lava_bucket": {
    "m": 1,
    "in": [[328, 1, {"consume": false}]],
    "out": [[325]],
    "dur": 1000,
    "eu": 2
  }
}
```

- `"consume": true` (default) — предмет расходуется целиком
- `"consume": false` — предмет не удаляется из контейнера (bucket → empty bucket через другой механизм)

### nbt_match (future)

Пока не реализуется. Для случаев: "рецепт требует алмазную кирку с прочностью > 80%".

---

## Service: RecipeManager

**RecipeManager** — C++ библиотека, embedded в SimulationCore (отдельный сервис не развёрнут, хотя протокол recipe.fbs определён).

### Загрузка

При старте сервиса загружает все рецепты из JSON-файлов, расположенных в `data/recipes/`. Каждый файл — объект, где ключ = ID рецепта. ID рецептов уникальны глобально (не только в пределах файла).

Пример структуры файла — см. секцию "Формат рецепта" выше. Валидация при загрузке:
- `item_id` должен быть uint16 (0–65535)
- duration ≥ 1
- output не пустой
- machine_type в диапазоне

**JSON → внутреннее представление**:
```cpp
struct Recipe {
    std::string id;             // ключ из JSON
    std::vector<ItemStack> inputs;
    std::vector<OutputItem> outputs;  // OutputItem = ItemStack + override поля
    Protocol::MachineType machine;
    uint32_t duration;
    float energy_cost;
};
```

ItemRegistry используется для name→ID конвертации при загрузке рецептов. JSON-формат использует числовые ID, а ItemRegistry::nameToId() — для резолва строковых имён, если они есть.

### В памяти

Все рецепты хранятся в `std::unordered_map<std::string, Recipe>` до остановки сервиса. Дополнительный индекс `recipesByMachineType_` для быстрой фильтрации. GC не требуется.

### RPC-интерфейс

| Метод | Параметры | Возврат | Описание |
|-------|-----------|---------|----------|
| `CheckRecipe` | `container: Container`, `machine_type: MachineType` | `recipe_id: string` | Проверяет, выполняется ли рецепт для данной машины с указанным содержимым. Возвращает ID рецепта или `null`. |
| `Craft` | `recipe_id: string`, `container: Container` | `new_container: Container` | Выполняет рецепт: забирает `inputs`, добавляет `outputs` (с NBT/overrides). Возвращает обновлённый контейнер. |
| `EvaluateConditions` | `recipe_id: string`, `machine_state: MachineState` | `bool` | Проверяет Layer 2 условия (температура, жидкости и т.д.). |

FlatBuffers schema — `protocol/recipe.fbs`. ItemStack внутри Container — `{item_id: uint16, count: uint8, meta: uint16}`. Имена предметов по wire не передаются.

### Интеграционные точки

- **SimulationCore** — CraftRequestHandler вызывает RecipeManager напрямую (embedded, без RPC).
- **Machines** (верстак) — вызывают `Craft` через Gateway → CraftRequestHandler → RecipeManager.
- **GameClient** — WorkbenchWindow с MatchPattern() preview (8 embedded patterns). CraftResponse handler в NetClient.

---

## Layer 2: Рецепты с условиями (GregTech-подобное расширение)

### Категории рецептов

Каждый рецепт имеет категорию, определяющую тип машины и набор поддерживаемых условий:

- **smelting** — плавильня (температура, энергия)
- **chemical_reactor** — химический реактор (жидкости, чистота, энергия)
- **machining** — фрезеровка (энергия, длительность)
- **assembly_line** — сборочная линия (энергия, жидкие компоненты)

### Дополнительные условия (Layer 2 поля в JSON рецепта)

```json
{
  "chem_oxygen": {
    "m": 5,
    "in": [[330]],
    "out": [[331]],
    "temp": {"min": 100, "max": 500},
    "liquid": [["water", 1000]],
    "purity": 0.95,
    "biome": ["ocean", "river"],
    "dur": 100,
    "eu": 30
  }
}
```

| Поле | Тип | Описание |
|------|-----|----------|
| `temp` | `{"min": number, "max": number}` | Температура в °C |
| `liquid` | `[[liquid_id, amount], ...]` | ID жидкости из registry + объём в mB |
| `purity` | `number` (0.0–1.0) | Требуемая чистота |
| `biome` | `string[]` | Список биомов |
| `special` | `string` | Слот для нестандартных условий |

### Проверка условий

✅ `ConditionEvaluator` реализован (152 строки). Проверяет environment (temperature, purity, biome), machine (energy, network, facing), special (key/value tags).

**Проблема**: вызывается с пустым `MachineState{}` — код проверки есть, данных нет. Нужно завязать на реальное состояние машины.

`EvaluateConditions` RPC (из recipe.fbs) не развёрнут — ConditionEvaluator вызывается напрямую через RecipeManager::evaluateConditions().

**MachineState** (передаётся через FlatBuffers):
```flatbuffers
table MachineState {
  temperature:float;
  liquid_levels:[Liquid];
  energy:float;
  purity:float;
  biome:string;
  duration:uint32;
}
```

---

## Открытые вопросы

1. ~~**Формат хранения JSON** — единый файл на сервис или разделение по машинам?~~ ✅ **Решено**: один файл на машину (`furnace.json`, `assembler.json`...).
2. ~~**Бинарный формат** — стоит ли перенести рецепты в FlatBuffers?~~ ✅ **Решено**: uint16 на wire достаточно. JSON для хранения — человекочитаем, diff-friendly.
3. **Температура** — как RecipeManager узнаёт текущую температуру? Через RPC от машины.
4. **Жидкости** — как RecipeManager узнаёт объём жидкости в машине?
5. **Чистота** — как RecipeManager узнаёт текущую чистоту?
6. **Биомы** — как RecipeManager узнаёт текущий биом машины?
7. **Сложные условия** — как реализовать `special` (игрок, время суток)?
8. **ItemRegistry sync** — как клиент получает свежий items.db? Встроить в бинарник? HTTP от MetaDB?
9. **ItemRegistry versioning** — что если ID предмета изменился между версиями модпака? Migration SQL?
10. **NBT в оверрайдах** — достаточно ли плоского JSON для NBT, или нужен полноценный NBT-формат?

---

## Примечания

- **RecipeManager** будет реализован на C++ с использованием стандартных контейнеров.
- JSON-парсинг выполняется с помощью `nlohmann/json` для быстрого MVP.
- RPC-интерфейс использует FlatBuffers для передачи `Container` и `MachineState`.
- ItemRegistry в SQLite — `sqlite3` C API (встроенный, без отдельного процесса).
- CSV → SQLite конвертируется скриптом сборки; CSV под гитом, SQLite — артефакт сборки.
