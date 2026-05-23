# User Flow диаграммы — GTNH Platform

Диаграммы пользовательских сценариев (userflow/usecases) для воксельной high-performance игры на C++ с автономной работой, управлением теплом, item/fluid/energy transport, вольтажем и мультиблоками.

## Структура

| Файл | Тема | Диаграмм |
|------|------|----------|
| `01-player-interaction.puml` | Размещение/удаление блоков (CAS), крафт, инвентарь, машины, мир | 5 |
| `02-autonomous-operation.puml` | Симуляция без игрока, буры, цепочка переработки | 3 |
| `03-heat-management.puml` | Heat Transfer, Boiler→Steam, охлаждение/перегрев | 3 |
| `04-machine-operations.puml` | Machine lifecycle (20 Hz), Energy flow, Multiblock formation | 3 |
| `05-system-network.puml` | Startup sequence, Service communication, Multiplayer sync | 3 |
| `06-item-energy-transport.puml` | Item pipes, Fluid pipes, Energy cables (вольтаж, tier) | 3 |
| `07-ore-processing-chain.puml` | Ore processing chain (руда→пластина), Voltage tiers & transformers | 2 |
| `08-wrench-tools-config.puml` | Wrench конфигурация сторон, Electric tools, Machine side setup | 3 |
| `09-multiblocks.puml` | EBF, Large Steam Boiler, Large Chemical Reactor | 3 |

**Всего: 28 диаграмм в 9 файлах**

## Генерация PNG

```bash
plantuml doc/userflow/*.puml -tpng
```

## Цветовая схема (согласована с C4)

| Цвет | Назначение |
|------|-----------|
| `#438D80` (teal) | Core C++ сервисы |
| `#5899DA` (blue) | Go sidecars / Steam / Fluids |
| `#9B59B6` (purple) | GameClient / Multiblocks |
| `#E67E22` (orange) | Energy / Tiers / Recipe |
| `#E74C3C` (red) | Ошибки / перегрев / explosions |

## Связанные C4 диаграммы

`doc/c4/level4-flows.puml` — системные event sequence диаграммы (ChunkLoading, Multiblock, Crafting, Inventory)
