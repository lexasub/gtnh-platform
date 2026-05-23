# Remaining Work — 2-infrastructure-shared-recipe-lib

This epic is archived as **fully implemented**.

## All 3 phases completed

| Phase | Описание | Статус |
|-------|----------|--------|
| Phase 1: Extract | `src/libs/recipe_manager_lib/` created with 9 files + CMakeLists.txt | ✅ Done |
| Phase 2: Migrate consumers | SimulationCore and recipe_manager link `recipe_manager_lib` | ✅ Done |
| Phase 3: Cleanup | Single source of truth — no duplicated RecipeManager code | ✅ Done |

## Что было сделано

- **`src/libs/recipe_manager_lib/`** — статическая библиотека:
  - `RecipeManager.h/.cpp` — загрузка, хранение, проверка рецептов
  - `ConditionEvaluator.h/.cpp` — проверка условий (temp, liquid, purity, biome)
  - `ItemRegistry.h/.cpp` — name↔ID конвертация
  - `RecipeConditions.h` — структуры условий
  - `RecipeTypes.h` — общие типы
  - `CMakeLists.txt` — сборка библиотеки
- **SimulationCore** — линкует `recipe_manager_lib`, удалил свой RecipeManager/
- **recipe_manager** — линкует `recipe_manager_lib`, удалил свой RecipeManager/
- **ItemRegistry singleton** — остаётся, работает в обоих сервисах

## Открытые вопросы (не критичные)

1. **Синхронизация рецептов** — оба сервиса грузят одни JSON-файлы. Ок для MVP.
2. **EnTT dependency** — библиотека тянет EnTT за собой. Для standalone recipe_manager это лишний вес. Возможное разделение на `recipe_manager_core` (без EnTT) и `recipe_manager_ecs` (с ConditionEvaluator) — отложено.

## Исходные файлы

Архив содержит оригинальный spec + tasks/.
