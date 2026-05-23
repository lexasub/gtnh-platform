# Luanti (Minetest) Pattern Analysis

**Epic name:** Luanti Pattern Analysis  
**Layer:** Layer 0 (Foundation) — кросс- cutting анализ  
**Status:** Analysis

## Основание

В ходе реверс-инжиниринга Luanti (Minetest, `SOUL2.md`) выявлены три подхода, которые могут улучшить архитектуру GTNH Platform:

| # | Паттерн | Luanti | У нас | Потенциал |
|---|---------|--------|-------|-----------|
| 1 | **Inventory Actions** | `IMoveAction`, `IDropAction`, `ICraftAction` — first-class сетевые протоколы с `allowPut/allowTake/allowMove` колбэками | `InventoryAction` в core.fbs есть, но Move/Drop/Craft как отдельные Table не выделены | Упростить клиент-серверную синхронизацию инвентаря |
| 2 | **Craft Replacements** | `CraftReplacements` — пара (убрать→добавить) для bucket→empty bucket | `consume: false` в input override | Обобщить до пар убираемых/добавляемых предметов |
| 3 | **Craft Priority System** | Приоритеты: `SHAPED > SHAPELESS > TOOLREPAIR > COOKING > FUEL`, хэш-индексация | Простой перебор `recipesByMachineType_` | Добавить priority + shaped/shaless типы |

## 1. Inventory Actions Protocol

### Luanti
- `IMoveAction` — переместить предметы между слотами (сервер проверяет через `allowMove`)
- `IDropAction` — выбросить предметы на землю (`allowTake`)
- `ICraftAction` — скрафтить предметы
- Каждый action имеет `apply()` сервер-сайд и `clientApply()` клиент-сайд
- Колбэки: `onTake/onPut/onMove` + `allowPut/allowTake/allowMove` (возврат negative = reject)

### У нас
`core.fbs` уже содержит `InventoryAction`:
```flatbuffers
table InventoryAction {
  player_id:uint64;
  action_type:uint8;       // 0=MOVE, 1=SPLIT, 2=DROP, 3=CRAFT
  source_slot:uint8;
  target_slot:uint8;
  count:uint8;
  meta:uint16;
}
```

### Что можно улучшить
- Разделить на отдельные Table `MoveItems`, `DropItems`, `CraftRequest` вместо одного `action_type:uint8`
- Добавить `InventoryActionResponse` с результатом (accepted/rejected/reason)
- Добавить колбэки `allowPut`/`allowTake` на стороне SimulationCore

### Ripple effect
- Gateway: новая обработка action types
- SimulationCore: `InventoryActionHandler` уже существует — расширить
- GameClient: drag-and-drop в SlotGrid уже есть — завязать на new actions
- MetaDB: `player_inventory` blob уже есть — принимать actions

### Risk
- Изменение wire protocol → сломает существующие клиенты
- Колбэки `allow*` добавляют latency на hot-path (но можно кэшировать)

## 2. Craft Replacements

### Luanti
`CraftReplacements` — вектор пар (itemstring to remove, itemstring to add). Пример: печка угля — ведро воды убирается, пустое ведро добавляется.

### У нас
Сейчас `consume: false` в input override:
```json
{
  "in": [[328, 1, {"consume": false}]]
}
```
`Recipe::craft()` просто пропускает items с `consume=false`. Ведро не сжигается, но пустое ведро и не добавляется — это баг/недоработка.

### Что можно улучшить
- Обобщить `consume: false` до списка replacements в формате рецепта:
```json
{
  "in": [[328, 1, {"replace": [[325]]}]]
}
```
Где `replace` — массив ItemStack, которые добавляются после крафта взамен убранных.
- В `Recipe::craft()`: после consumption, для каждого input с `replace`, добавить указанные предметы в контейнер.

### Ripple effect
- RecipeManager: изменить `Recipe::craft()` и парсинг input overrides
- JSON рецепты: обновить furnace.json (lava_bucket, water_bucket)
- ConditionEvaluator: не затронут

### Risk
- Минимальный — обратно совместимо (старые рецепты без `replace` работают как раньше)

## 3. Craft Priority System

### Luanti
Приоритеты (low→high):
1. `NO_RECIPE` (0)
2. `TOOLREPAIR` (1)
3. `SHAPELESS_AND_GROUPS` (2)
4. `SHAPELESS` (3)
5. `SHAPED_AND_GROUPS` (4)
6. `SHAPED` (5)

Хэш-индексация: `CRAFT_HASH_TYPE_ITEM_NAMES` (exact items, fastest), `CRAFT_HASH_TYPE_COUNT` (fallback), `CRAFT_HASH_TYPE_UNHASHED`.

### У нас
```cpp
std::unordered_map<Protocol::MachineType, std::vector<std::string>> recipesByMachineType_;
```
Простой вектор, матчинг линейным перебором. Нет приоритетов, нет shaped/shaless типов.

### Что можно улучшить
- Добавить `priority` поле в рецепт (uint8, default=0)
- В `Recipe::matches()` — при равном матчинге выбирать по highest priority
- Добавить `RecipeType` enum: `SHAPED` (фиксированная сетка) vs `SHAPELESS` (любой порядок)
- Хэш-таблица для быстрого lookup по item_id (item_names hash)

### Ripple effect
- `Recipe` struct: new fields `priority`, `recipe_type`
- `RecipeManager::findRecipeByInputs()`: сортировать по priority
- Парсинг JSON: новые поля
- MachineSystem: не затронут (просто получает recipe_id)

### Risk
- Шейпед-рецепты с фиксированной сеткой (3x3) — потребуют изменения протокола `Container` (сейчас просто вектор 9 ItemStack)
- Шейплесс — можно без изменений протокола

## Affected Services

| Service | Inventory Actions | Craft Replacements | Craft Priority |
|---------|-------------------|--------------------|----------------|
| **core.fbs** | ✅ MoveItems/DropItems Table | ❌ | ✅ RecipeType enum |
| **recipe.fbs** | ❌ | ❌ | ✅ priority field |
| **Gateway** | ✅ relay | ❌ | ❌ |
| **SimulationCore** | ✅ InventoryActionHandler | ❌ | ✅ findRecipeByInputs |
| **GameClient** | ✅ SlotGrid / drag-drop | ❌ | ❌ |
| **RecipeManager** | ❌ | ✅ craft() / parse | ✅ priority/type |
| **data/recipes/** | ❌ | ✅ furnace.json | ✅ новые поля |

## Decision Summary

| Паттерн | Когда делать | Блокеры |
|---------|-------------|---------|
| **Inventory Actions** (first-class протокол + `allow*` колбэки) | ❌ Post-MVP — после полной реализации inventory chain и протокол версионирования | Breaking change wire protocol; 🟡 inventory chain не завершён; требует version negotiation в протоколе |
| **Craft Replacements** (`consume: false` → `replace`) | ✅ **Сейчас** — малый scope, 0 breaking changes, обратно совместимо | Две реализации RecipeManager (simulation_core + recipe_manager сервис) — нужно менять обе |
| **Craft Priority** (только `priority:uint8`) | ⚠️ После Craft Replacements | Дублирование RecipeManager; shaped/shapeless отложены (требуют изменения Container в протоколе) |

### Порядок реализации

```
1. Craft Replacements      → сейчас
2. Craft Priority (uint8)   → после п.1
3. Inventory Actions        → post-MVP
```

### Что НЕ делать в этой итерации

- Shaped/shapeless типы рецептов — отложить до полноценной системы крафта (меняет Container протокол)
- Hash-индексация рецептов — преждевременная оптимизация для <100 рецептов
- Inventory Actions — не ломать wire protocol, пока inventory chain не реализован end-to-end

## Следующие шаги

1. Устранить дублирование RecipeManager (simulation_core vs standalone recipe_manager сервис)
2. Craft Replacements — task spec + реализация
3. Craft Priority — task spec + реализация

## Критерии готовности анализа

- [ ] Каждый паттерн оценён на применимость (да/нет с обоснованием)
- [ ] Для принятых паттернов: task spec с изменениями протокола, сервисов и данных
- [ ] Оценён ripple effect по affected services
- [ ] CI не ломается (новые поля опциональны)

## Открытые вопросы

1. Стоит ли внедрять все 3 паттерна сразу или по одному?
2. Inventory Actions — это breaking change для wire protocol. Делать в мажорной версии протокола?
3. Craft Priority — нужны ли shaped рецепты на MVP? Или только shapeless + priority?
