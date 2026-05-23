# REMAINING_WORK.md — Luanti Pattern Analysis

**Архивирован**: 2026-06-20
**Статус**: Анализ завершён, все реализуемые паттерны обработаны

## Что было сделано

| Паттерн | Статус | Куда ушло |
|---------|--------|-----------|
| **Craft Replacements** | ✅ **Уже реализовано** в `recipe_manager_lib` | `parseCompactInputList()` парсит `replace`/`replace_meta`, `Recipe::craft()` применяет замену при `!consume` |
| **Craft Priority** | ⏳ Deferred | Опциональная оптимизация для <100 рецептов — не критично |
| **Inventory Actions** | ❌ Post-MVP | Breaking change wire protocol — после полной реализации inventory chain |

## Причина архивации

EPIC содержал analysis 3 паттернов. Craft Replacements уже были реализованы в коде (EPIC содержал устаревшую информацию о баге). Остальные 2 паттерна — deferred до Post-MVP или оптимизация.
