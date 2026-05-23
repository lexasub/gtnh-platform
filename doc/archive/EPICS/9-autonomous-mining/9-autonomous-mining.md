# EPIC: Autonomous Mining

**Layer**: L2  
**Статус**: 🟡 **WIP — DrillSystem MVP DONE**  
**Последнее обновление**: 2026-06-28 — DrillSystem полностью имплементирован (ECS компонент + система, BFS поиск руды, прогресс добычи, output buffer, энергопотребление)  
**Зависимости**: PipeNetwork (энергия), EntityStateStore (персистентность), MachineRegistry (типы дрелей)

## Userflow диаграммы

- `doc/userflow/02-autonomous-operation.puml` — A2: Autonomous drilling

## Обзор

Автономное бурение — ключевая возможность GTNH: игрок ставит блок дрели, подключает энергию, и дрель автоматически ищет руду под собой, добывает её и складывает в output buffer.

## Текущее состояние (из кода): **MVP DONE**

### Реализовано (2026-06-28)

| Компонент | Статус | Описание |
|-----------|--------|----------|
| DrillComponent | ✅ DONE | ECS компонент: позиция, tier, energyPerTick, searchLayer/Index, target, miningProgress, outputBuffer |
| DrillSystem | ✅ DONE | 20 Hz tick: energy check → spiral BFS → mining progress → output |
| Spiral BFS | ✅ DONE | Асинхронный поиск руды спиралью через GetBlock RPC, 2 блока/тик |
| Mining progress | ✅ DONE | Прогресс добычи на основе tier дрели (ULV=40t, LV=20t, MV=10t, HV=5t) |
| Output buffer | ✅ DONE | Буфер на 64 слота, OUTPUT_FULL состояние при заполнении |
| Energy consumption | ✅ DONE | Энергопотребление через PipeEnergyClient, sendConsumeRequest |
| Drill types | ✅ DONE | 4 типа в consumers.csv + items.csv (ULV=61/100, LV=62/101, MV=63/102, HV=64/103) |
| Block entity update | ✅ DONE | Публикация прогресса для клиентского UI |
| Ore detection | ✅ DONE | 10 типов руды: iron, gold, quartz, electrum, tin, uranium, coal, redstone, lapis, diamond |

### Не реализовано (pending)

| Фича | Причина |
|------|---------|
| Persistence через EntityStateStore | MVP: при рестарте дрели начинают с нуля |
| Integration с ItemPipe (автовыгрузка) | Нужен ItemPipe имплементация в PipeNetwork |
| SpatialIndex вместо прямых GetBlock | SpatialIndex не реализован |
| Multidimension drilling | Пока только 0-dimension |
| Клиентский UI для дрели | Нужен MachineWindow для drill block |

## Блоки (Drill machine types)

| Block ID | Название | Tier | Радиус | Макс глубина | EU/tick | EU/block |
|----------|----------|------|--------|-------------|---------|---------|
| 61 (consumers) | drill_ulv | ULV (0) | 10 | 64 | 4 | 10 |
| 62 | drill_lv | LV (1) | 10 | 64 | 8 | 20 |
| 63 | drill_mv | MV (2) | 10 | 64 | 16 | 30 |
| 64 | drill_hv | HV (3) | 10 | 64 | 32 | 40 |

## Item IDs

| Item ID | Название |
|---------|----------|
| 100 | gtnh:drill_ulv |
| 101 | gtnh:drill_lv |
| 102 | gtnh:drill_mv |
| 103 | gtnh:drill_hv |

## Архитектура

```
DrillSystem::tick() — 20 Hz
  ├─ DrillState::IDLE → SEARCHING
  ├─ DrillState::SEARCHING → phaseSearch()
  │   └─ getSpiralOffset(n) → getBlock(wx,wy,wz) RPC
  │   └─ onSearchBlockResult → isOreBlock → MINING
  ├─ DrillState::MINING → phaseEnergyCheck() + phaseMine()
  │   ├─ energy.consumeEnergy(energyPerTick)
  │   ├─ miningProgress--; if 0 → setBlockCAS to AIR
  │   └─ onMineComplete → oreToDrop → outputBuffer
  └─ DrillState::OUTPUT_FULL → ждать освобождения буфера
```

## Файлы

| Файл | Роль |
|------|------|
| `ECS/components/DrillComponent.h` | Компонент с состоянием дрели |
| `ECS/Systems/DrillSystem.h` | Заголовок системы |
| `ECS/Systems/DrillSystem.cpp` | Имплементация (280 строк) |
| `data/registry/consumers.csv` | Типы дрелей (ID 61-64) |
| `data/registry/items.csv` | Предметы дрелей (ID 100-103) |