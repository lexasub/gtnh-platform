# Shared RecipeManager Library

**Epic name:** Shared RecipeManager Library  
**Layer:** Layer 0 (Foundation) — рефакторинг инфраструктуры  
**Status:** Draft

## Проблема

`RecipeManager` — код для загрузки, хранения и проверки рецептов — существует в двух копиях:

| Копия | Путь | Использование |
|-------|------|--------------|
| SimulationCore inline | `src/services/simulation_core/RecipeManager/` | Hot path — 20 Hz, ECS MachineState, тысячи машин |
| Standalone RPC | `src/services/recipe_manager/RecipeManager/` | Client-side queries (recipe book, поиск) |

**Последствия дублирования:**
- Изменения в одном не синхронизируются с другим
- Разные источники истины для рецептов
- Удвоенный объём компиляции
- Баги при загрузке рецептов (форматы, пути к ItemRegistry)

## Решение

Вынести общий код в статическую библиотеку `src/libs/recipe_manager_lib/`. SimulationCore и recipe_manager линкуют её как `target_link_libraries(... recipe_manager_lib)`.

```
src/
└── libs/
    └── recipe_manager_lib/
        ├── CMakeLists.txt
        ├── RecipeManager.h          ← единый Header
        ├── RecipeManager.cpp
        ├── ConditionEvaluator.h
        ├── ConditionEvaluator.cpp
        ├── ItemRegistry.h
        ├── ItemRegistry.cpp
        └── RecipeConditions.h
```

### Что остаётся в сервисах

| Компонент | Остаётся в SimulationCore | Остаётся в recipe_manager |
|-----------|--------------------------|--------------------------|
| `evaluateConditions(reg, x, y, z)` с ECS | ✅ | ❌ |
| FlatBuffers RPC dispatch | ❌ | ✅ |
| CraftRequestHandler (NetClient) | ✅ | ❌ |
| RecipeManagerService (Subscribe/Publish) | ❌ | ✅ |
| Machine tick loop | ✅ | ❌ |

## Affected Services

| Service | Layer | Изменение |
|---------|-------|-----------|
| **recipe_manager_lib** ⬅️ **NEW** | L0 | Статическая библиотека — общий код RecipeManager, ConditionEvaluator, ItemRegistry |
| **SimulationCore** | L1 | Удалить `RecipeManager/` из своего `CMakeLists.txt`, линковать `recipe_manager_lib` |
| **recipe_manager** ⬅️ **NEW** | L0 | Удалить `RecipeManager/` из своего `CMakeLists.txt`, линковать `recipe_manager_lib` |

## План миграции

### Phase 1: Extract (Library Creation)

1. Создать `src/libs/recipe_manager_lib/`
2. Скопировать из simulation_core:
   - `RecipeManager.h/.cpp`
   - `ConditionEvaluator.h/.cpp`
   - `ItemRegistry.h/.cpp`
   - `RecipeConditions.h`
3. Написать `CMakeLists.txt` для библиотеки
4. Заменить `#include "ItemRegistry.h"` пути на абсолютные от `src/libs/recipe_manager_lib/`
5. **Ничего не менять** в именах классов и API — только перемещение файлов

### Phase 2: Migrate Consumers

1. **SimulationCore**: удалить `RecipeManager/` из `target_sources`, добавить `target_link_libraries`
2. **recipe_manager**: удалить `RecipeManager/` из `target_sources`, добавить `target_link_libraries`
3. Проверить include paths в обоих `CMakeLists.txt`

### Phase 3: Cleanup

1. Удалить `src/services/simulation_core/RecipeManager/`
2. Удалить `src/services/recipe_manager/RecipeManager/`
3. Убедиться, что `src/libs/recipe_manager_lib/` — единственный источник

## Схема линковки

```
SimulationCore                     recipe_manager (reciped)
       │                                  │
       └──────────┬───────────────────────┘
                  │
       ┌──────────▼──────────┐
       │ recipe_manager_lib  │
       │ (STATIC library)    │
       └─────────────────────┘
                  │
       ┌──────────▼──────────┐
       │ External deps       │
       │ (spdlog, EnTT,      │
       │  nlohmann_json)     │
       └─────────────────────┘
```

## Зависимости библиотеки

| Зависимость | Причина |
|-------------|---------|
| spdlog | Логирование загрузки и ошибок |
| nlohmann_json | Парсинг JSON рецептов |
| EnTT | ConditionEvaluator (ECS MachineState) |
| FlatBuffers (generated) | `recipe_generated.h` для Protocol::Container |

## Критерии готовности

- [ ] SimulationCore компилируется и работает с `recipe_manager_lib`
- [ ] recipe_manager компилируется и работает с `recipe_manager_lib`
- [ ] Все тесты проходят (существующие тесты рецептов)
- [ ] Нет дублирования файлов RecipeManager в сервисах
- [ ] CI проверяет, что `src/libs/` не содержит дубликатов

## Открытые вопросы

1. **Синхронизация рецептов** — как оба сервиса будут получать обновления? (Сейчас оба грузят одни те же JSON-файлы, это ок для MVP)
2. **ItemRegistry singleton** — библиотека использует `ItemRegistry::instance()`. Это нормально, пока всё в одном процессе. Для раздельных процессов — надо думать о синхронизации.
3. **EnTT dependency** — библиотека тянет EnTT за собой. Для standalone recipe_manager (где ConditionEvaluator с ECS не используется) это лишний вес. Возможное решение: разделить на `recipe_manager_core` (без EnTT) и `recipe_manager_ecs` (с ConditionEvaluator). Отложено.
