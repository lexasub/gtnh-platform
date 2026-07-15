# Анализ: что взять из Mechaenetia и «сессионного инженерного сурвайвала»

**Дата**: 2026-07-15  
**Источники**: 
- Концепция из диалога (Minecraft-кубы + DayZ дефицит + Tarkov сессии + GregTech инженерия)
- https://mechaenetia.com/gameplans/ (GregoriusT, автор GregTech-6)

**Цель**: Определить реализуемые идеи для gtnh-platform

---

## Что уже есть в gtnh-platform

| Механика | Состояние |
|----------|-----------|
| Кубы/модули | ✅ Чанки 32³, блоки, multiblock-паттерны |
| Инженерные цепочки | ✅ PipeNetwork (BFS, energy/fluid), RecipeManager |
| Энергосистема | ✅ CableGraph, overheat/explosion, heat transfer |
| ECS симуляция | ✅ EnTT, 20Hz tick, MultiblockController |
| Инвентарь | 🟡 Протокол + MetaDB + EntityStateStore, drag-and-drop частично |
| Крафтинг | 🟡 Workbench crafting end-to-end, 6 типов рецептов |
| Добыча руд | ✅ OreGenerator (GTNH-style veins, 3D Simplex noise) |
| Дефицит/сессии | 🔴 Отсутствует |
| Износ/усталость | 🔴 Только overheat, нет износа деталей |

---

## Архитектурное решение: среда

**Ключевое решение**: Отдельный сервис среды НЕ нужен.

```
WorldGenerator: генерирует humidity_map, temperature_map per chunk
        ↓ (при генерации чанка)
SpatialIndex: хранит humidity/temperature maps + radiation_zones
        ↓ (RPC: GetChunkMetadata)
SimulationCore: системы читают данные → применяют к entities
        ↓
  ├─ ApplyCorrosion system (humidity + material → corrosion)
  ├─ ApplyRadiationDamage system (zones → damage)
  └─ ApplyTemperatureEffects system (temperature → efficiency)
        ↓
MachineComponent (fatigue, wear, corrosion, quality)
        ↓
PipeNetwork (BFS для fluids/energy,不变)
        ↓
GameClient (visual warnings, instinct menu suggestions)
```

**Почему не отдельный сервис**:
- Environmental effects happen TO entities → это ECS системы
- SpatialIndex уже отвечает за spatial queries
- SimulationCore поддерживает multi-tick-rate
- PipeNetwork = BFS, коррозия не влияет на flow solving

---

## Идеи для реализации (по применимости)

### 🔴 Высокая применимость (расширяют существующие системы)

#### 1. Износ и усталость материалов

**Идея**: Не просто «прочность», а «усталость». Машина, долго работавшая на пределе, ломается чаще.

**Реализация**:
- `MachineComponent`: `fatigue_accumulator`, `stress_history[]`, `wear_rate`
- Каждый тик: `fatigue += (load / max_load)^2 * dt`
- Вероятность поломки: `failure_chance = base_rate * fatigue * (1 + stress_cycles / fatigue_limit)`
- Визуализация: ImGui gauge «усталость»

**Сервисы**: SimulationCore, GameClient, EntityStateStore

---

#### 2. Коррозия от среды

**Идея**: Инструменты/машины ржавеют во влажной среде.

**Реализация**:
- `MachineComponent`: `corrosion_rate: f32`, `corrosion_type: enum(rust, rot, chemical)`
- **SpatialIndex**: humidity_map per chunk
- **SimulationCore**: ApplyCorrosion system
- Факторы: humidity, chemical_exposure, blood_contact

**Сервисы**: SimulationCore, SpatialIndex, MachineComponent

---

#### 3. Radiation System

**Идея**: Alpha, Beta, Gamma, Neutron, UV, Microwave, X-Ray — каждый тип по-разному влияет.

**Реализация**:
- Protocol: enum RadiationType
- **SpatialIndex**: radiation_zones (источник + радиус + тип)
- **SimulationCore**: ApplyRadiationDamage system
- Защита: свинец → Alpha/Beta, бетон → Gamma, водород → Neutron

**Сервисы**: SimulationCore, SpatialIndex, Protocol, GameClient

---

#### 4. Humidity/Temperature Zones

**Идея**: Влажные зоны → коррозия. Горячие зоны → перегрев.

**Реализация**:
- **WorldGenerator**: генерация humidity/temperature per chunk
- **SpatialIndex**: хранение (не ChunkStore — компактный формат)
- **SimulationCore**: ApplyTemperatureEffects system
- CorrosionSystem: влажность > 0.7 → corrosion_rate *= 2.0

**Сервисы**: WorldGenerator, SpatialIndex, SimulationCore, GameClient

---

#### 5. Сезоны погоды

**Идея**: Зима, весна, лето, осень — каждая меняет temperature/humidity.

**Реализация**:
- **WorldGenerator**: генерирует seasonal_modifier per chunk
  - temperature_base + seasonal_offset (зимой -20%, летом +20%)
  - humidity_base + seasonal_offset (зимой сухо, летом влажно)
- **SpatialIndex**: хранит current_season + seasonal_modifiers
- **SimulationCore**: ApplySeasonalEffects system
  - зима: коррозия ускоряется (влажность + холод), замерзшие трубы
  - лето: перегрев ускоряется (температура + влажность), испарение
  - осень: дожди → максимальная влажность, fungal growth
  - весна: таяние снега → затопление, muddy terrain

**Геймплей**:
- Зима сложнее = больше вызовов для инженера
- Сезоны = динамика. Мир не статичный
- Атмосфера: снег, дождь, туман

**Сервисы**: WorldGenerator, SpatialIndex, SimulationCore, GameClient

---

### 🟡 Средняя применимость (новые UI/системы, но архитектура готова)

#### 6. Сессионные зоны с эвакуацией

**Идея**: Отдельные «экспедиционные» чанки. Всё, что поставил — либо вывезти, либо потерять.

**Реализация**:
- ChunkStore: тип чанка `persistent` / `session`
- Session-чанки: LMDB отдельно, при эвакуации — снапшот в MetaDB
- MessageRouter: topic `session.evacuate`

**Сервисы**: ChunkStore, MetaDB, MessageRouter, Gateway, GameClient

---

#### 7. Лимиты на транспортировку модулей

**Идея**: Слоты инвентаря не только для предметов, но и для модулей/кубов.

**Реализация**:
- `Inventory`: `module_capacity`, `size_class` per module
- При эвакуации: проверка capacity → overflow = потеря

**Сервисы**: SimulationCore, MetaDB, GameClient

---

#### 8. Цепные аварии

**Идея**: Перегрев → пожар → повреждение соседних → потеря данных.

**Реализация**:
- HeatTransferSystem: `fire_spread_chance` при 100% overheat
- `adjacent_damage` при взрыве
- Визуализация: bgfx heatmap overlay

**Сервисы**: SimulationCore, GameClient

---

#### 9. Multi-tick rates

**Идея**: Не всё должно тикаться на 20Hz. Некоторые вещи тикают раз в секунду/минуту.

**Реализация**:
- SimulationEngine: `tick_rate` per system (fast/normal/slow/event-only)
- PipeNetwork: уже кэширует 5 секунд → расширить
- HeatTransferSystem: 5Hz, DrillSystem: 20Hz

**Сервисы**: SimulationCore

---

#### 10. Action Menu

**Идея**: Вместо кучи биндов — контекстное меню для блока/предмета.

**Реализация**:
- ImGui: radial/text menu для текущего блока
- MachineWindow: встроить action menu
- WrenchHandler: меню выбора вместо циклического переключения

**Сервисы**: GameClient

---

#### 11. Instinct Menu

**Идея**: Меню предлагает действия по ситуации. Горишь → «перекатиться». Темно → «включить фонарик».

**Реализация**:
- SimulationCore: публикует `state_alerts`
- GameClient: ловит alerts → показывает Instinct Menu

**Сервисы**: SimulationCore, GameClient, MessageRouter

---

### 🟢 Низкая применимость (требуют существенных изменений)

#### 12. NPC Workers

**Идея**: NPC делают рутину: фермерствуют, охраняют, строят, торгуют.

**Реализация**:
- SimulationCore: NPC entity + AI state machine
- MetaDB: NPC persistence
- AFK mode: игрок = NPC пока не в сети

**Сервисы**: SimulationCore, MetaDB

---

#### 13. Research/Technology System

**Идея**: Знания в книгах/свитках. Книжная полка = быстрый доступ к рецептам.

**Реализация**:
- MetaDB: `known_recipes: HashSet<RecipeID>`
- SimulationCore: bookshelf entity → area_of_effect
- RecipeManager: фильтрация по known_recipes

**Сервисы**: MetaDB, SimulationCore, RecipeManager

---

## Дополнительные идеи (из Mechaenetia)

> Эти идеи не вошли в основной список по применимости, но могут быть полезны в будущем.

### 0.25m Blocks + Megablocks

**Идея**: Блоки 0.25m³ (в 64 раза мельче Minecraft). Megablocks 16x16x16.

**Реализация**:
- Chunk format: 32³ блоков по 0.25m = 8m³ чанк
- Megablock detection: 16x16x16 одинаковых → один entity
- Collision: один AABB вместо 4096 проверок

**Сервисы**: ChunkStore, SimulationCore, GameClient

---

### Octree Chunk System

**Идея**: Дерево решений для чанков. Однородные регионы = листья.

**Реализация**:
- ChunkStore: octree metadata per chunk
- Lazy loading: только видимые ветки
- Collision: octree traversal

**Сервисы**: ChunkStore, SimulationCore

---

### Tool Durability System

**Идея**: 4 типа durability: Sharpness, Handle, Corrosion, Rot.

**Реализация**:
- MachineComponent: `sharpness`, `handle_integrity`, `corrosion`, `rot`
- Repair: grindstone, rehandle, chemical wash, replace
- Tool heads reversible: turn pickaxe around

**Сервисы**: SimulationCore, GameClient

---

### Effect Types

**Идея**: 26 типов эффектов (A-Z): яды, аллергии, паразиты, вирусы.

**Реализация**:
- Protocol: EffectType enum
- SimulationCore: duration, intensity, resistance
- Лечение: specific cure items per type

**Сервисы**: SimulationCore, Protocol, MetaDB

---

### Resource Complexity

**Идея**: 5 уровней сложности от «просто Money» до «реальный GregTech».

**Реализация**:
- RecipeManager: переключение рецептов по difficulty
- Data: recipes.easy.json, recipes.normal.json, recipes.hard.json

**Сервисы**: RecipeManager, data/recipes, GameClient

---

### Tech Ages

**Идея**: Stone → Copper → Iron → Medieval → Early Industrial → Late Industrial → ...

**Реализация**:
- MetaDB: `current_age: enum`
- RecipeManager: рецепты привязаны к эпохе
- Unlock conditions: «построил furnace → открылся Copper Age»

**Сервисы**: MetaDB, RecipeManager, WorldGenerator

---

### Day/Night Cycle

**Идея**: 5 мин рассвета → 10 мин дня → 10 мин ночи → 5 мин заката.

**Реализация**:
- SimulationCore: `day_cycle_timer` per dimension
- Protocol: `time_of_day: f32`, `sun_angle: f32`
- GameClient: динамическое освещение

**Сервисы**: SimulationCore, Protocol, GameClient

---

### Diagonal Light Maps

**Идея**: Солнце светит не только сверху вниз. Боковые тени.

**Реализация**:
- WorldGenerator: sunlight_map + moonlight_map per chunk
- GameClient: bgfx shadow mapping

**Сервисы**: WorldGenerator, GameClient

---

### Fluid Physics

**Идея**: Вода течёт, пар поднимается, дым рассеивается.

**Реализация**:
- SimulationCore: fluid dynamics per-tick (1Hz)
- GameClient: particle effects

**Сервисы**: SimulationCore, PipeNetwork, GameClient

---

### Connected Trees

**Идея**: Срубил дерево → целое дерево с ветвями.

**Реализация**:
- SimulationCore: tree entity = parent + children
- GameClient: procedural tree model

**Сервисы**: SimulationCore, ChunkStore, GameClient

---

## Порядок реализации (по применимости)

```
ВЫСОКАЯ ПРИМЕНИМОСТЬ (расширение существующих систем):
1. Износ и усталость          → SimulationCore (расширить)
2. Коррозия от среды          → SimulationCore + SpatialIndex
3. Radiation System           → SimulationCore + SpatialIndex + Protocol
4. Humidity/Temperature       → WorldGenerator + SpatialIndex + SimulationCore
5. Сезоны погоды              → WorldGenerator + SpatialIndex + SimulationCore
6. Цепные аварии              → HeatTransferSystem (расширить)
7. Multi-tick rates           → SimulationCore

СРЕДНЯЯ ПРИМЕНИМОСТЬ (новые UI/системы):
8. Сессионные зоны            → ChunkStore + MetaDB + MessageRouter
9. Лимиты транспорта          → Inventory + SimulationCore
10. Action Menu               → GameClient
11. Instinct Menu             → SimulationCore + GameClient

НИЗКАЯ ПРИМЕНИМОСТЬ (требуют существенных изменений):
12. NPC Workers               → SimulationCore + MetaDB (долгосрочно)
13. Research System           → MetaDB + RecipeManager (долгосрочно)
```

**Логика**: Начать с высокой применимости (1-7) — это расширение существующих систем. Потом средняя (8-11) — новые UI/системы. Потом низкая (12-13) — долгосрочные проекты.

---

## Сравнение: gtnh-platform vs Mechaenetia

| Аспект | Mechaenetia | gtnh-platform |
|--------|-------------|---------------|
| Размер блока | 0.25m³ | 1m³ |
| Оптимизация | Octree + Megablocks | 32³ chunks |
| Форма мира | Donut/Torus | Плоский (пока) |
| Освещение | Diagonal light maps | Базовый |
| Деревья | Connected structures | Блоки |
| Инструменты | 4 типа durability | 1耐久-bar |
| NPC | Полная система | Нет |
| Исследование | Books/Libraries | Нет |
| Бой | Multi-skill | ECS (EnTT) |
| Модели | Кастомные размеры | Фиксированные |
| **Среда** | **Собственный сервис** | **SpatialIndex + SimulationCore** |

---

## Вывод

Из Mechaenetia и концепции «сессионного инженерного сурвайвала» реально взять **20+ конкретных механик**, которые усиливают gtnh-platform без перестройки архитектуры.

**Ключевые заимствования**:
1. **Износ/усталость** — делает инженерию осмысленной
2. **Коррозия + Radiation** — среда влияет на оборудование
3. **Multi-tick rates** — экономия CPU
4. **Action/Instinct Menu** — улучшение UX

**Архитектурное решение**: Отдельный сервис среды НЕ нужен. Данные в SpatialIndex, логика в SimulationCore.

Все реализуемы в текущем стеке (C++/Go, FlatBuffers, LMDB, EnTT) без новых зависимостей.
