# Инфраструктура, автоматизация и MVP — Продолжение

**Эпик**: Infrastructure, Automation & MVP — Continuation
**Слой**: Layer 3 (Automation) — отложено
**Статус**: Continuation (Parts A-D, G: Steps 1-6 done; See archive)

## Related Epics

| Epic | Layer | Status |
|------|-------|--------|
| [0-basic-mechanics](../0-basic-mechanics/basic-mechanics.md) | L1 | ✅ Done (core protocol, UI components) |
| [4-entitystatestore](../4-entitystatestore/entitystatestore.md) | L0 | ✅ Done |
| [5-recipe-manager](../5-recipe-manager/recipe-manager.md) | L0 | ✅ Done |
| [6-metadb](../6-metadb/metadb.md) | L0 | ✅ Done |
| [7-simulation-core](../7-simulation-core/simulation-core.md) | L1 | ✅ Done |
| [8-game-client](../8-game-client/game-client.md) | L1 | ✅ Done |

## Note on Completed Infrastructure

EntityStateStore and RecipeManager infrastructure is now done through sibling Layer 0 services. This continuation spec focuses on remaining automation features and world generation.

---

## Раздел E: WorldGenerator — генерация руд

Руды как блоки нужны для простых машин (печка, дробилка).

### Требования
- **Генерация**: простая синусоидальная жила (не сложная система жил GTNH)
- **Без инструментов**: любой предмет/рука ломает блок, предмет сразу в инвентарь
- **Стеки**: руды должны падать как обычные блоки

### Вопросы
- **Какие руды генерировать?** — open. Нужно выбрать набор для тестирования базовых машин.

---

## Раздел F: Автоматизация и логистика (Layer 3 — отложено)

### F.1 Автоматический ввод/вывод

**Что**: перемещение предметов между инвентарями (хопперы, трубы предметов).

**Архитектура**: компонент `ItemTransporter` в ECS. SimulationCore проверяет соседние инвентари, перемещает предметы согласно правилам (стороны, фильтры).

**Примитивы**: транспорт предметов — сейчас отсутствует.

### F.2 Логистические сетикуафсещкштп

**Что**: AE2-подобная система запросов предметов, приоритеты, крафт по требованию.

**Отложено**. Архитектурная закладка: инвентари должны иметь уникальные идентификаторы в ECS для обращений по сети.

### F.3 Редстоун / управляющие сигналы

**Решение**: НЕ ДЕЛАТЬ. Только если когда-нибудь игра станет вполне играбельной.

Блоки-источники сигнала изменяют meta блока. SimulationCore подписывается на изменения, передаёт в соседние машины.

---

## Новый EPIC по рудам

Генерация руд выделена в отдельный эпик:
👉 `doc/EPICS/6-world-ore-generation/6-world-ore-generation.md`

## Открытые вопросы

| Q# | Вопрос | Статус |
|----|--------|--------|
| Q3 | **ImGui синхронизация** — частота обновления GUI машин (каждый тик? раз в секунду?) | ⏸️ Deferred to 100+ machines milestone |
| Q4 | **Какие руды генерировать?** | ✅ **Решено** — см. `6-world-ore-generation` |
