# C4 Architecture Diagrams — GTNH Platform

Ресурсивные C4-диаграммы для распределённой Minecraft-платформы.

## Структура файлов

```
doc/c4/
├── _common.puml                        # Общие настройки, цвета, теги
├── level1-context.puml                 # L1 — System Context
├── level2-container.puml               # L2 — Containers (13 сервисов)
│
├── level3-simcore.puml                 # L3 — SimulationCore overview (подсистемы)
├── level3-client.puml                  # L3 — GameClient overview (подсистемы)
├── level3-chunkstore.puml              # L3 — ChunkStore (LMDB, ClockCache, async I/O)
├── level3-gateway.puml                 # L3 — Gateway (ctrl + bulk порты)
├── level3-messagerouter.puml           # L3 — MessageRouter (priority channels, MQTT)
├── level3-entity-state-store.puml      # L3 — EntityStateStore (C++ :5200, LMDB)
├── level3-meta-db.puml                 # L3 — MetaDB (Go — SQLite, CRUD, event handlers)
├── level3-pipe-network.puml            # L3 — PipeNetwork (BFS, flow distribution)
├── level3-recipe-manager.puml          # L3 — RecipeManager (standalone service)
├── level3-tiny-services.puml           # L3 — WorldGenerator + SpatialIndex + Validation
│
├── level4-sim-ecs-core.puml            # L4 — SimCore: Engine + 6 systems + Actions
├── level4-sim-ecs-components.puml      # L4 — SimCore: 14 ECS data-компонентов
├── level4-sim-crafting.puml            # L4 — SimCore: Crafting + RecipeManager + Inventory
├── level4-sim-network.puml             # L4 — SimCore: Network clients + Storage repos
├── level4-sim-world.puml               # L4 — SimCore: World + Multiblock patterns + Common
│
├── level4-client-core.puml             # L4 — Client: Core + Infra + Camera + Common
├── level4-client-network.puml          # L4 — Client: NetClient + IoUring + очереди
├── level4-client-world.puml            # L4 — Client: World + Cache + MeshManager
├── level4-client-render.puml           # L4 — Client: Render + RenderLib (bgfx, ImGui)
├── level4-client-ui-core.puml          # L4 — Client: UI Core (UIManager, Input, Crafting DBs)
├── level4-client-ui-windows.puml       # L4 — Client: Windows + Panels + SlotGrid/NEI
│
├── level4-deployment.puml              # L4 — Deployment: ports, DBs, topic map
└── level4-flows.puml                   # L4 — Key event flows (4 диаграммы)
```

## Уровни C4

| Уровень | Имя | Что показывает |
|---------|-----|---------------|
| 1 | **System Context** | Игрок, администратор, внешние системы |
| 2 | **Container** | 13 сервисов, TCP соединения, MessageRouter pub/sub |
| 3 | **Component (overview)** | Подсистемы крупных сервисов (SimCore, Client) + детали остальных |
| 4 | **Component (detail)** | Внутреннее устройство подсистем — классы, компоненты, потоки данных |

## Как генерировать

### С PlantUML (локально)

```bash
# Установка
pip install plantuml
# или
sudo apt install plantuml

# Генерация PNG одного файла
plantuml doc/c4/level2-container.puml -tpng

# Генерация всех диаграмм
plantuml doc/c4/*.puml -tpng
```

### С PlantUML (Java)

```bash
# Скачать plantuml.jar
wget https://github.com/plantuml/plantuml/releases/latest/download/plantuml.jar

# Генерация
java -jar plantuml.jar doc/c4/level2-container.puml -tpng
```

### Онлайн

Открыть `.puml` файлы на [plantuml.com/plantuml](https://www.plantuml.com/plantuml/) или
использовать плагины для VS Code ("PlantUML" by jebbs / "PlantUML Preview").

## Цветовая схема

| Цвет | Назначение |
|------|-----------|
| `#438D80` (teal) | Core C++ сервисы |
| `#5899DA` (blue) | Go sidecars |
| `#9B59B6` (purple) | Игровой клиент |
| `#E67E22` (orange) | Протокол / FlatBuffers |
| `#2C3E50` (dark) | Базы данных |
| `#BDC3C7` (grey) | Планируемые / заглушки |
| `#95A5A6` (light grey) | Внешние системы |

## Типы связей

| Линия | Тип | Описание |
|-------|-----|----------|
| Сплошная синяя | TCP | Прямое TCP соединение |
| Пунктирная фиолетовая | Pub/Sub | Через MessageRouter |
| Сплошная зелёная | RPC | Request-Response |
| Пунктирная оранжевая | Protocol | FlatBuffers обмен |
| Пунктирная серая | Planned | Планируемая функциональность |
