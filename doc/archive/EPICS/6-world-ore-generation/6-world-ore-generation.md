# EPIC: World Generation — Ore Veins

**Layer**: L1  
**Статус**: 🔴 Не начато  
**Base**: Section E of `3-infrastructure-automation`  
**Зависимости**: WorldGenerator (существующий C++ сервис), data/registry

## Userflow диаграммы

- `doc/userflow/07-ore-processing-chain.puml` — O1: Ore processing chain (macerator→furnace→compressor)

## Обзор

После плоского мира (MVP) нужно добавить генерацию руд — простые синусоидальные жилы для тестирования машин (печка, дробилка, компрессор).

---

## Что нужно сделать

### 1. Типы руд (из реального items.csv)

Базовый набор для тестирования цепочки переработки. Уже зарегистрированы в `data/registry/items.csv`:

| Ore | Block ID в items.csv | Цель |
|-----|----------------------|------|
| Iron Ore | 3 | Основная руда → пластины |
| Gold Ore | 5 | Кабели (MV) |
| Tin Ore | 26 | Кабели (ULV), бронза |
| Copper Ore | 53 | Кабели (LV), бронза |
| Coal Ore | (добавить) | Топливо для генераторов |
| Redstone Ore | (добавить) | Цепи, компоненты |
| Lapis Ore | (добавить) | Магические компоненты |
| Diamond Ore | (добавить) | Продвинутые инструменты |

### 2. Генерация жил

Простая синусоидальная жила (не сложная система GTNH):
- 3D синусоида: `ore(x,y,z) = sin(ax) * sin(by) * sin(cz)` > threshold
- Жила идёт через несколько блоков (не кластер)
- Толщина жилы: 1–3 блока
- Ore density: chance per block в жиле

**Пример алгоритма:**
```cpp
// В WorldGenerator::generateChunk(chunk_x, chunk_z)
for (x = 0; x < 32; x++) {
    for (z = 0; z < 32; z++) {
        for (y = min_y; y < max_y; y++) {
            float noise = noise3D(world_x, y, world_z, seed);
            if (noise > threshold && y in vein_height[ore_type]) {
                setBlock(x, y, z, ore_block_id);
            }
        }
    }
}
```

### 3. Конфигурация жил

Каждая руда должна иметь:
- block_id
- min_y / max_y (высотный диапазон)
- threshold (порог шума — контролирует плотность)
- frequency (частота жилы)
- vein_size (толщина)

Формат: `data/registry/ores.json` или CSV

---

## Где смотреть

| Файл | Роль |
|------|------|
| `src/services/world_generator/WorldGenerator.h` | GenerateFlat(), GenerateTerrain(), GenerateChunk() |
| `src/services/world_generator/WorldGenerator.cpp` | **Реальная генерация**: шум, слои, ore (lines 127-132) |
| `src/services/world_generator/GenerationQueue.h/.cpp` | Асинхронная очередь генерации чанков |
| `src/services/world_generator/CMakeLists.txt` | **Важно**: worldgeneratord — **библиотека**, не сервис |
| `src/services/chunk_store/World/ServerWorld.cpp` | Интеграция: ServerWorld вызывает WorldGenerator |
| `src/services/chunk_store/CMakeLists.txt` | chunkd линкует worldgeneratord |
| `data/registry/items.csv` | Регистрация ore item_id (iron_ore=3, gold_ore=5, tin_ore=26, copper_ore=53) |

**Ключевое открытие**: WorldGenerator — **не standalone service**. Нет main.cpp, нет MessageRouter. Это библиотека, линкуется в `chunkd`. `ServerWorld::loadOrGenerateChunk()` вызывает `generator_->GenerateTerrain()`.

## Текущий код генерации

**WorldGenerator.cpp использует:**
- `glm::perlin()` для 2D terrain height
- `FastNoise::Simplex` fractal через `GenUniformGrid3D()` для пещер и руд
- Scale: caves=0.05f, ore=0.1f (один октав)

**Генерация руд сейчас (line 127-132):**
```cpp
if (oreNoise > 0.7) {
    block_id = 5;  // ХАРДКОД — gold_ore
}
```

**Проблемы:**
- ❌ Только один тип руды (block_id=5)
- ❌ Не использует items.csv registry
- ❌ Один порог (0.7) для всех
- ❌ Без привязки к высоте
- ❌ Нет конфигурации per ore type

## Архитектура (реальная)

**WorldGenerator → ChunkStore flow:**
1. `ServerWorld::loadOrGenerateChunk(chunk_pos)` — чанка нет в LMDB
2. `GenerationQueue::enqueue(chunk_pos, callback)` — асинхронно
3. `WorldGenerator::GenerateTerrain(chunk, chunk_x, chunk_z)` — рельеф + руды
4. callback оповещает ServerWorld — чанк готов
5. Чанк сохраняется в LMDB, публикуется `world.chunks.data`

**Взаимодействие с машинами:**
- Руды добываются вручную (любой предмет/рука — без инструментов MVP)
- Руды падают как блоки (stackable)
- Macerator: руда → 2 crushed
- Furnace: crushed → ingot
- Compressor: ingot → plate

## Открытые вопросы

| Q# | Вопрос | Решение |
|----|--------|---------|
| Q1 | Какие руды генерировать | Железо, медь, олово, уголь, золото, редстоун, лазурит, алмаз |
| Q2 | Одинаковое или разное количество | Iron > Copper > Tin > Coal > Gold > редкие |
| Q3 | Размер жилы | 1–5 блоков (тонкие) |
| Q4 | Требуются ли инструменты для добычи | Нет — MVP: любой предмет/рука |

## Критерии готовности

- [ ] Ore item_id в items.csv/items.db (не менее 4: iron, copper, tin, coal)
- [ ] Ore vein config (ores.json или inline в WorldGenerator)
- [ ] Синусоидальная генерация: 3D noise → threshold → ore block
- [ ] Разные высоты для разных руд (iron: y<40, coal: y<80, copper: y<60, tin: y<50)
- [ ] WorldGenerator публикует SetBlock для руд при генерации чанка
- [ ] Без инструментов: руда добывается рукой
- [ ] ChunkStore сохраняет руду в LMDB
