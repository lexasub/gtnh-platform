# План: генерация деревьев (oak) → добыча oak_planks

**Статус**: черновик v3 (ревью №2: блокер клиентского крафта закрыт, предикат унифицирован, SurfaceHeights обязателен).
**Тип**: feature (issue: "oak planks unobtainable through gameplay").
**Acceptance**: игрок может получить oak_planks в survival-геймплее; деревья генерируются естественно и при ломании падает log.
**Решения (v2)**: плотность = вероятностная, крона = эллипсоид, только oak, листья непрозрачные, чинить dirt/grass, OpenSpec proposal.

---

## 1. Текущее состояние (что уже есть)

### 1.1 WorldGenerator (`src/services/world_generator/`)

- **`WorldGenerator::GenerateTerrain(Chunk&, cx, cy, cz)`** — `WorldGenerator.cpp:84`. Чистая функция, вызывается из `GenerationQueue` (8 потоков, `GenerationQueue.h:25`), каждый чанк генерится **независимо**, состояние — только `thread_local`.
- Алгоритм (`WorldGenerator.cpp:84-141`):
  1. 2D Perlin+FBM (`baseFBM`, `contFBM`) → heightmap `heights[]` (32×32 float). Формула: `BASE_HEIGHT(64) + baseNoise*BASE_AMP(12) + contNoise*CONT_AMP(20)` (`WorldGenerator.cpp:95`). **Диапазон высот: [32, 96]** (64 ± 12 ± 20). FBM-ноды — в file-local `anonymous namespace` (`WorldGenerator.cpp:36-43`, `thread_local`) — наружу не экспортируются.
  2. 3D Simplex+FBM (`caveFBM`) → пещеры.
  3. Заполнение блоков: `STONE=0:0:1`, `DIRT=0:0:7`, `GRASS=0:0:8`, `WATER=1111:11:0`, `AIR=0:0:0` (`WorldGenerator.cpp:20-24`). **Песка/гравия в террейне нет вообще.** Вода заливается только при `worldY < 0` (`WorldGenerator.cpp:117-118`) — при высотах [32,96] **недостижимо** → океанов в текущем террейне нет, water-guard мёртв (см. §4.4).
  4. `oreGen.generateOres(...)` — GTNH-жилы (см. ниже).
- **`OreGenerator`** (`OreGenerator.h/.cpp`) — образец стиля:
  - Детерминизм: `hashRegion(rx, rz, seed)` (`OreGenerator.cpp:24`) — чистая функция координат, регион 3×3 чанка, пересечения решаются проверкой 3×3 соседних регионов (`OreGenerator.cpp:69`).
  - Заменяет **только STONE** (`OreGenerator.cpp:158: if (currentBlock != 1) continue`).
- **`GenerationQueue`** — пул из 8 воркеров; порядок чанков произвольный → межчанковые зависимости запрещены.

### 1.2 Реестр блоков/предметов

- `data/registry/items.csv` — источник истины. Загружается `ItemRegistry::loadFromCSV` (`src/libs/recipe_manager_lib/ItemRegistry.cpp:16`, вызывается в `simulation_core/main.cpp:126`).
- Формат id — иерархический, пакуется `ItemId::pack` (`src/common/ItemId.h:85`): сегменты до последнего `:` — бинарные префикс-биты, последний — payload.
- **Дерево (BASE wood, префикс `0:10`) сейчас** (`items.csv:12-21`):
  - `0:10:00:0` oak_planks (`0x4000`), `0:10:00:1` spruce_planks, `0:10:00:2` birch_planks
  - `0:10:01:0` oak_slab, `0:10:01:1` spruce_slab
  - `0:10:10:0` oak_door, `0:10:10:1` spruce_door
  - `0:10:11:0` chest, `0:10:11:1` crafting_table
- **НЕТ блоков log и leaves вообще.** Oak-пластины есть, но их не из чего получить.
- `oak_planks` — ингредиент многих рецептов (`data/recipes/crafting_table.yaml:11-177`) и quests (`data/quests/quests.csv:4`).

### 1.3 Рецепты — **ДВА механизма, оба обязательны** (блокер v3)

| Механизм | Файл | Роль | Детали |
|---|---|---|---|
| Сервер-авторити | `data/recipes/crafting_table.yaml` | валидация CraftRequest в RecipeManager | 3×3 паттерн, по **имени** (`oak_planks`), пустая `~` |
| Клиентский превью-гейт | `src/services/game_client/Crafting/ClientRecipeDB.h:22-111` — `constexpr Recipe kRecipes[]` | кнопка крафта активна только при совпадении | 13 хардкод-рецептов, `ItemId::pack(...)` |

Поток крафта (подтверждено кодом):
1. `CraftingGrid::Recalc()` → `Crafting::MatchGrid(slots_.data(), &result_)` (`CraftingGrid.cpp:75`).
2. `MatchGrid` (ClientRecipeDB.cpp:5-22) — позиционное сравнение item_id по 9 слотам против `kRecipes`.
3. Кнопка/отправка `SendCraftRequest` — только если `grid_.GetResult().item_id != 0` (`ClientCraftingWindow.cpp:108-110`).

**Вывод**: рецепт `oak_log → oak_planks` обязан жить **в обоих местах** (yaml для сервера + kRecipes для клиента), паттерн — позиционно идентичный. Только yaml = кнопка мертва, крафт невозможен (§5).

⚠️ `generator.yaml:19-25` и `boiler.yaml:23-29` ссылаются на `{ item: 13 }` (`# oak_planks`) — старые flat id, сломаны. Не трогаем; новый рецепт — **только по имени**.

### 1.4 Дроп при ломании блока — УЖЕ РАБОТАЕТ, НЕ ТРОГАЕМ

- `simcore` → `SetBlockCASHandler::handle` (`src/services/simulation_core/Actions/SetBlockCASHandler.cpp:165`): LEFT_CLICK → CAS на AIR → `onGiveItem_(player_id, broken_block, 1, -1)` (`SetBlockCASHandler.cpp:251-256`).
- Дроп-таблиц нет, блок падает как есть. **simcore сам решает, какой блок отдать, по id кликнутого блока** — подтверждено пользователем, менять ничего не нужно.

### 1.5 Клиентский рендер

- `ChunkMeshBuilder::GetBlockColor(uint16_t blockId)` — `src/services/game_client/Render/ChunkMeshBuilder.cpp:14-21`, `static constexpr` switch на `ItemId::pack(...)`; default = белый.
- Сейчас: `0:0:1` stone, `0:0:2` (комментарий "dirt"), `0:0:3` (комментарий "grass").
- ⚠️ **Баг**: в реестре `0:0:2` = cobblestone, `0:0:3` = sand, а dirt/grass = `0:0:7`/`0:0:8`. Реальная трава/земля рендерятся **белыми**. **Чиним в этой же задаче.**

### 1.6 Персистентность мира

- ChunkStore пишет чанки в LMDB (`./chunkdb`, `src/services/chunk_store/main.cpp:28`). Сгенерированные чанки **не перегенерируются** → для визуальной проверки `rm -rf ./chunkdb`.

---

## 2. Ключевое архитектурное решение: per-column детерминизм

### Проблема
Чанки генерируются независимо на 8 потоках. Крона (радиус 2) и окно локального максимума (5×5) пересекают границы чанка — оба чанка должны поставить согласованно одни и те же блоки.

### Решение
Всё — чистая функция координат `(x, z, seed)` (как `hashRegion` у руд). **Единый предикат `isTreeAt`** — один источник истины для стволов И крон:

1. **Предрасчёт** (`§4.2`): `hmap_` (высоты 36×36), `scoreMap_` (деревья-оценка), `treeMap_` (итог: ствол есть/нет) для области чанка + запас 2.
2. **Ствол**: для каждой колонки чанка: `treeMap_` → да → ствол `y = surface+1 .. surface+h` → `BLOCK_LOG` (clamp `[baseY, baseY+32)`).
3. **Крона**: для каждой колонки чанка: деревья из `treeMap_` в радиусе 2 → `inCanopy` → `BLOCK_LEAVES` **только на AIR**. Соседний чанк вычислит то же → границы согласованы.

### 2.1 Единый предикат (v3 — одна версия вместо трёх)

Ревью №2: в v2 предикат был определён тремя разными способами (§2.1 `%100`, §2.2 `>>16 × density`, §4.5 «по hmap_ + плотности + localmax`). **Унификация: один скаляр `treeScore`** — и кандидатство, и локальный максимум сравнивают ровно его:

```cpp
// TreeGenerator.h
uint32_t hashTree(int32_t x, int32_t z) const;   // как hashRegion, seed = SEED_TREES (отдельный оффсет!)
float    density  (int32_t x, int32_t z) const;  // 0.5 + 0.5*forestFBM(...) — кластеризация

// ЕДИНЫЙ критерий дерева: базовая вероятность, масштабированная плотностью.
// Плотность входит В score (не в отдельный гейт) — localmax сравнивает тот же score.
inline float treeScore(int32_t x, int32_t z) const {
    float chance  = (hashTree(x, z) & 0xFFFF) / 65535.0f;  // [0..1) — равномерный
    return chance * density(x, z);                          // [0..1) — плотный центр выше
}

// Порог: дерево ставится, если score выше порога И score — строгий локальный максимум в 5×5.
inline bool isTreeAt(int32_t x, int32_t z) const {
    float s = treeScore(x, z);
    if (s < TREE_SCORE_THRESHOLD) return false;             // кандидат (включает density)
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx) {
            if (dx == 0 && dz == 0) continue;
            if (treeScore(x + dx, z + dz) > s) return false; // сосед «сильнее» — уступаем
        }
    return true;
}
```

- `treeScore` — единственный критерий: **плотность гейтит кандидатов тем же числом, что сравнивает локальный максимум** → нет расхождения `%100` vs `>>16` (п.3 ревью).
- Локальный максимум → расстояние между стволами ≥ 3 (Chebyshev) → кроны (R=2) не образуют сплошной потолок.
- Эффективная плотность ограничена окном: ~1 ствол на 5×5 в плотном пятне. Ожидаемое поведение, кластеры мягкие (см. §10.Q2).

### 2.2 Биомы не нужны (MVP): noise-кластеризация

Полная биом-система — overkill. Достаточно **третьего FBM-слоя низкой частоты** `forestFBM` (по аналогии `contFBM`): `density ∈ [0.5, 1]` — значение шума масштабирует score (не бинарный «лес/не лес»). Плавные кластеры вместо резкой «стены леса». При будущих биомах `TreeGenerator` получает тот же сигнал из `BiomeProvider` — интерфейс не меняется.

---

## 3. Новые блоки (items.csv)

```csv
0:10:11:2,oak_log,,0
0:10:11:3,oak_leaves,,0
```

- `0:10:11:*` — подпрефикс misc (0=chest, 1=crafting_table, свободны 2+). Оба в `CAT_BASE`.
- **ID подтверждены вычислением**: сегменты "0","10","11" = биты `01011` (5 бит) → `prefix=11, shift=11` → `11<<11 = 0x5800`; `pack("0:10:11:2") = 0x5802`, `pack("0:10:11:3") = 0x5803`. Конфликтов с существующими нет (payload 0/1 заняты).
- **Статическая проверка** (не «руками»): `static_assert` в тесте клиента:

```cpp
// game_client/tests/test_client.cpp (или test_item_registry.cpp)
static_assert(ItemId::pack("0:10:11:2") == 0x5802u, "oak_log id drift");
static_assert(ItemId::pack("0:10:11:3") == 0x5803u, "oak_leaves id drift");
static_assert(ItemId::pack("0:10:11:2") != ItemId::pack("0:10:11:0")
           && ItemId::pack("0:10:11:2") != ItemId::pack("0:10:11:1"), "payload collision");
```

---

## 4. TreeGenerator (новый файл, стиль OreGenerator)

Файлы: `src/services/world_generator/TreeGenerator.h/.cpp` + **`SurfaceHeights.h/.cpp`** (см. §4.6). Добавить оба в `world_generator/CMakeLists.txt` (`add_library(worldgeneratord ...)`, `CMakeLists.txt:12-16`). C++26, `-O3 -ffast-math` уже настроены.

### 4.1 Stateless + thread_local буферы

`GenerateTerrain` зовётся из 8 потоков. **Никакого мутабельного состояния в объекте** — только `const uint32_t m_seed_`. Все буферы — `thread_local` внутри `generateTrees` (как `WorldGenerator.cpp:46-49`, `OreGenerator.cpp:16-18`). Объект на стеке в `GenerateTerrain`.

### 4.2 Предрасчёт сеток 36×36 (перф + консистентность)

Шаг кроны наивно = 32×32×5×5×FBM — сотни тысяч операций на чанк. Решение — один предрасчёт с запасом 2 (footprint ≤ 2):

```cpp
// thread_local внутри generateTrees
thread_local std::array<float, 36*36> hmap_;     // высоты (SurfaceHeights, §4.6)
thread_local std::array<float, 36*36> scoreMap_; // treeScore для той же области
thread_local std::array<bool,  36*36> treeMap_;  // isTreeAt ∧ slopeOK — единственный источник

// 1) hmap_: 36×36 = 1296 колонок × 2 FBM (base+cont) = 2592 сэмпла — из SurfaceHeights (§4.6),
//    GenerateTerrain берёт ту же сетку для 32×32 — пересчёта нет.
// 2) scoreMap_: 1296 × (1 hashTree + 1 forestFBM) ≈ 2600 операций.
// 3) treeMap_: 1296 × 25 сравнений score (локальный max 5×5) + slopeOK ≈ 32k сравнений — дёшево.
// 4) Шаги 2-3 читают сетки O(1), без повторного шума/хешей.
```

- `heightAt(tx,tz)` → `hmap_[...]`, `isTreeAtCached` → `treeMap_[...]`.
- Достаточно запаса **2**: крона радиусом 2 и окно localmax 5×5. Большие поля не нужны.
- `treeMap_` — единственный источник для стволов И крон → консистентность структурная, не по договорённости.

### 4.3 Форма кроны: эллипсоид

```cpp
// Trunk top T = floor(surface) + h. Центр кроны = (tx, T - 1, tz).
bool inCanopy(int32_t dx, int32_t dz, int32_t wy, int32_t centerY) {
    if (dx == 0 && dz == 0 && wy <= centerY + 1) return false;   // ствол не перезаписываем
    float ex = dx * dx + dz * dz;                                  // / R_h^2 (4)
    float ey = (wy - centerY) * (wy - centerY);                    // / R_v^2 (2.25)
    return (ex / 4.0f) + (ey / 2.25f) <= 1.0f;
}
```

- Цикл по колонке: `wy ∈ [baseY, baseY+32)`, деревья в радиусе 2. `R_h=2, R_v=1.5` — тюнинг-константы (§4.7).

### 4.4 Проверка поверхности: ТОЛЬКО симметричные чистые функции (v3)

Правило (ревью №2, п.2): **любая проверка должна быть одинаковой с обеих сторон границы либо отсутствовать**. Проверка типа блока из своего чанка асимметрична (соседний чанк в `GenerationQueue` ещё не сгенерирован) → «осиротевшие кроны». Поэтому:

- **В предикате** (зашито в `treeMap_`, единый источник — шаги ствола и кроны читают его):
  - **slope-guard**: `|hmap_[p] − hmap_[neighbor]| ≤ SLOPE_MAX` (4 соседа). Высоты — чистая функция координат → симметричен. **Один и тот же guard для ствола и кроны** — баг v2 (крона без slope-guard → плавающая крона на склоне в центре чанка) исключён структурно.
- **Убран water-guard**: вода в текущем террейне недостижима (высоты [32,96], заливка при `worldY < 0`, §1.1) → guard был мёртвым кодом, тест «нет деревьев в воде» непротестируем. Настоящий уровень воды + деревья у побережий — отдельная задача (см. §9).
- Поверхность террейна = только STONE/DIRT/GRASS (песка нет) → деревья всегда на траве/земле; slope режет крутые обрывы.
- **Phase 2**: генерация террейна 3×3 до деревьев (read-only буфер) → честный `blockAt == GRASS || DIRT`. Требует менять `GenerationQueue` (независимость потоков) — не «лёгкая» задача, не в MVP.

### 4.5 Псевдокод generateTrees (v3)

```cpp
// TreeGenerator.h
class TreeGenerator {
public:
  explicit TreeGenerator(uint32_t seed);
  void generateTrees(Chunk& c, const SurfaceHeights& surf,
                     int baseX, int baseY, int baseZ, int chunkSize = 32);
private:
  uint32_t hashTree(int32_t x, int32_t z) const;
  float    density  (int32_t x, int32_t z) const;
  float    treeScore(int32_t x, int32_t z) const;
  bool     isTreeAt (int32_t x, int32_t z) const;   // score + localmax (без высот)
  const uint32_t m_seed_;
};
```

```text
generateTrees(c, surf, baseX, baseY, baseZ, size):
    if (max(hmap_) < baseY || min(hmap_) > baseY + size):
        return                                    // чанк далеко от поверхности — ранний выход

    // 1. Предрасчёт (thread_local) — §4.2
    hmap_    = surf.fill(baseX-2, baseZ-2, 36)    // SurfaceHeights, чистый вызов
    scoreMap_ = treeScore(...) для 36×36
    treeMap_  = isTreeAt(...) && slopeOK(hmap_, ...) для 36×36

    // 2. Стволы — только колонки текущего чанка
    for z in 0..31, x in 0..31:
        if !treeMap_[(z+2)*36 + (x+2)]: continue
        surface = floor(hmap_[...])
        h = 4 + (hashTree(baseX+x, baseZ+z) >> 8) % 4        // 4..7
        for wy in surface+1 .. surface+h:
            writeBlock(x, wy, z, BLOCK_LOG)                  // clamp [baseY, baseY+32)

    // 3. Кроны — деревья из treeMap_ в радиусе 2 от колонок чанка
    for z in 0..31, x in 0..31:
        for dz in -2..2, dx in -2..2:
            gx = x+2+dx, gz = z+2+dz
            if !treeMap_[gz*36 + gx]: continue
            tx = baseX+x+dx, tz = baseZ+z+dz
            tSurface = hmap_[gz*36 + gx]
            hT = 4 + (hashTree(tx, tz) >> 8) % 4
            centerY = floor(tSurface) + hT - 1
            for wy in max(baseY, centerY-2) .. min(baseY+32, centerY+2):
                if inCanopy(x+dx, z+dz, wy, centerY) && block(x, wy, z) == AIR:
                    writeBlock(x, wy, z, BLOCK_LEAVES)
```

Замечания:
- Крона читает `treeMap_` (ствол реально существует в этой колонке, с учётом slope) → «осиротевшая крона» невозможна.
- Стволы пишутся **без проверки AIR** (могут «врасти» в скалу на 1 блок при slope-границе) — допустимо, см. тест §7.6. Листья — только на AIR.
- `hashTree`/`isTreeAt`/`hmap_` — чистые → повторяемость из любого чанка, включая вертикальную границу (cx,cy)/(cx,cy+1): каждый чанк пишет свою часть ствола через clamp, объединение непрерывно **только при идентичной формуле высот** → SurfaceHeights обязателен (§4.6).

### 4.6 SurfaceHeights — ОБЯЗАТЕЛЬНЫЙ общий модуль (v3, был «опциональным»)

Проблема: FBM-ноды (`baseFBM`, `contFBM`) — file-local в `anonymous namespace` (`WorldGenerator.cpp:36-43`), наружу не экспортируются. TreeGenerator обязан повторить конфиг (scale, octaves, seeds) и формулу `64 + base*12 + cont*20` — классический **drift-риск**: малейшее расхождение → ствол в воздухе или в земле, и вертикальные границы разъезжаются (см. §4.5).

**Решение — новый модуль** `SurfaceHeights`:

```cpp
// SurfaceHeights.h — единственный владелец формулы высот
class SurfaceHeights {
public:
  explicit SurfaceHeights(uint32_t seed);        // владеет baseFBM/contFBM (переехали сюда)
  void   fill(float* out, int size, int baseX, int baseZ) const;  // grid + margin
  float  at(int32_t x, int32_t z) const;
private:
  FastNoiseLite baseFBM_, contFBM_;              // больше не file-local в WorldGenerator.cpp
};
```

- `WorldGenerator::GenerateTerrain` **переписывается** на `SurfaceHeights` (убирает свою копию формулы, ноды из anonymous namespace удаляются).
- `TreeGenerator` принимает `const SurfaceHeights&` → 36×36 сетка из того же источника. Пересчёта 32×32 нет — GenerateTerrain берёт сетку из `fill()`.
- Одна формула в кодовой базе → drift исключён. Это **обязательная часть задачи**, не фаза 2.

### 4.7 Константы (v3 — все числа заданы)

```cpp
// TreeGenerator.h
inline static constexpr uint32_t SEED_TREES       = 0x5EED; // оффсет hashTree — НЕ пересекается с hashRegion (SEED+12345)
inline static constexpr float    TREE_SCORE_THRESHOLD = 0.12f; // базовая доля кандидатов
inline static constexpr float    SLOPE_MAX        = 1.5f;  // макс. перепад высот между соседними колонками
inline static constexpr int      MAX_TREE_H       = 7;     // высота ствола: 4 + (hash>>8)%4 → [4..7]
// forestFBM: FastNoiseLite(seed + 0xF0F0), Perlin, 3 октавы, freq FOREST_SCALE
inline static constexpr float    FOREST_SCALE     = 0.004f;// пятна ~250 блоков
// SurfaceHeights: те же scale/octaves/seed, что сейчас в WorldGenerator.cpp:36-43 (SEED / SEED+1) — переносим как есть
// Блоки: constexpr как в WorldGenerator.cpp:20-24
inline static constexpr uint32_t BLOCK_LOG    = ItemId::pack("0:10:11:2"); // 0x5802
inline static constexpr uint32_t BLOCK_LEAVES = ItemId::pack("0:10:11:3"); // 0x5803
```

⚠️ `SEED_TREES` — **отдельный seed-оффсет**, не совпадает с `hashRegion` (руд): иначе распределения деревьев и руд коррелируют.

---

## 5. Рецепт: oak_log → oak_planks — **В ДВУХ МЕСТАХ** (блокер)

### 5.1 Сервер (`data/recipes/crafting_table.yaml`)

```yaml
  - name: oak_log_to_planks
    pattern:
      - [oak_log, ~, ~]
      - [~, ~, ~]
      - [~, ~, ~]
    inputs:
      - { item: oak_log, count: 1 }
    outputs:
      - { item: oak_planks, count: 4 }
    duration: 100
    min_tier: 0
    max_tier: 32767
```

Только по имени, без числовых id (§1.3).

### 5.2 Клиентский превью-гейт (`ClientRecipeDB.h` — 14-й рецепт в `kRecipes[]`)

```cpp
// base:oak_log_to_planks — 1 oak_log → 4 oak_planks
{{{{ItemId::pack("0:10:11:2"), 1}, {}, {},
   {}, {}, {},
   {}, {}, {}}},
 {ItemId::pack("0:10:00:0"), 4}},
```

- `MatchGrid` — **позиционное** сравнение (ClientRecipeDB.cpp:5-22) → паттерн обязан совпасть с yaml 1:1 (log в верхнем-левом слоте, остальное пусто).
- Сервер остаётся авторитетом: `CraftRequest` валидируется в RecipeManager по yaml. kRecipes — только клиентское превью (активация кнопки). Если разойдутся — превью врёт, но сервер отклонит.
- Тест на MatchGrid — §7.8.

---

## 6. Клиент: цвета блоков

`ChunkMeshBuilder::GetBlockColor` (`ChunkMeshBuilder.cpp:14-21`):

```cpp
case ItemId::pack("0:10:11:2"): return 0xFF8B5A2B;  // oak_log (коричневый)
case ItemId::pack("0:10:11:3"): return 0xFF228B22;  // oak_leaves (зелёный)
case ItemId::pack("0:0:7"):     return 0xFF8B5A2B;  // dirt  (БАГ: сейчас маппится на 0:0:2)
case ItemId::pack("0:0:8"):     return 0xFF50AF4C;  // grass (БАГ: сейчас маппится на 0:0:3)
// удалить неверные case 0:0:2 (cobblestone), 0:0:3 (sand) — либо дать им свои цвета
```

- `ItemId::pack` в `case` — безопасно: уже используется в этом же switch (`ChunkMeshBuilder.cpp:16-18`), `pack` полностью `constexpr` (`ItemId.h:85`). Никаких runtime-строк.
- Листья — **непрозрачные** (сплошной куб). Alpha-cutout = отдельная задача (у `ChunkMeshBuilder` есть `transparentVertices`, `ChunkMeshBuilder.cpp:56-57`).

---

## 7. Тесты

Паттерн: `ctest` (`CMakeLists.txt:23`), пример `simcored_test` (`src/services/simulation_core/CMakeLists.txt:213`). Для world_generator — новый тест-таргет `worldgeneratord_test`.

1. **Детерминизм**: `generateTrees` дважды с тем же seed → идентичные блоки (полный diff чанка).
2. **Горизонтальная граница**: `(cx,cz)` и `(cx+1,cz)`, колонки x=31/32: листья/ствол согласованы, нет обрывов кроны.
3. **Вертикальная граница** (v3): `(cx,cy)` и `(cx,cy+1)` — ствол, пересекающий y=baseY+32, непрерывен (нижний чанк пишет низ, верхний — верх; объединение = целый ствол). Ловит drift формулы высот (§4.6).
4. **Разреженность стволов**: нет двух стволов на расстоянии < 3 (Chebyshev) в области 3×3 чанков.
5. **Ствол на земле**: под стволом не-AIR; ствол начинается с `surface+1`.
6. **Инвариант не-AIR** (v3, вместо пустого теста «листья не перезаписывают руду»): сгенерить чанк с рудами → подсчёт не-AIR блоков до/после `generateTrees` **равен** (деревья пишут только в AIR; стволы — только в столбце над землёй; руды живут внутри STONE и не пересекаются).
7. **Нет крон без ствола** (v3 — ловит асимметрию §4.4): для области 3×3 чанков каждая колонка с листьями имеет ствол дерева (колонка из `treeMap_` с `wy ∈ [T-h, T]`) в радиусе 2. Если slope-guard в предикате и в кроне разойдётся — тест падает.
8. **Рецепты**:
   - yaml: (по образцу `test_recipe_manager.cpp`) `oak_log` → `oak_planks` x4 резолвится по имени.
   - kRecipes: в `game_client/tests/test_client.cpp` — `Crafting::MatchGrid({log,0,0,...})` → output `{0x4000, 4}`.
   - `static_assert` id — §3.
9. ~~Нет деревьев в воде~~ — **удалён** (воды в террейне нет, §4.4).

Ручная проверка:
- `rm -rf ./chunkdb` (старые чанки не регенерируются, §1.6).
- Запустить стек: леса на поверхности, границы без «обрезанных» крон.
- Сломать ствол → в инвентаре `oak_log`; положить в crafting table (сетка 3×3) → кнопка крафта активна → `oak_planks` x4.
- Земля/трава перестали быть белыми (§6).

---

## 8. Что НЕ менять

- `SetBlockCASHandler` / дроп — работает, simcore сам решает (подтверждено пользователем, §1.4).
- `BlockType.h` клиента — старая flat-схема, к террейну отношения не имеет.
- `generator.yaml`/`boiler.yaml` с `item: 13` — отдельная (пред-существующая) проблема.
- `OreGenerator`/`OreConfig` — не трогаем.

---

## 9. Фаза 2 (опционально, НЕ в этой задаче)

- **Уровень воды + деревья у побережий** (сейчас вода недостижима, §4.4).
- **Биом-система**: `BiomeProvider` → палитра блоков, плотность леса. TreeGenerator уже принимает `density`-сигнал.
- **Разные деревья**: spruce/birch (`0:10:11:4+`, конус/другие параметры).
- **Честная проверка поверхности**: террейн 3×3 до деревьев → `blockAt == GRASS/DIRT`. Требует изменений в `GenerationQueue` (конфликт с независимой параллельной генерацией) — не «лёгкая» задача.
- **Дроп-таблицы**: листья → шанс саженец, посадка/рост.
- **trees.json** по аналогии `ores.json` (плотность/высоты/радиусы).
- **Alpha-cutout листья**.

---

## 10. Решённые вопросы (v2) и ответы на ревью №2 (v3)

**v2**: (1) плотность вероятностная; (2) крона эллипсоид; (3) только oak; (4) листья непрозрачные; (5) dirt/grass чиним; (6) OpenSpec proposal.

**Ревью №2 — вердикт по каждому пункту**:
1. **kRecipes — блокер, закрыт**: подтверждено кодом (`ClientRecipeDB.h:22-111`, `CraftingGrid.cpp:75`, `ClientCraftingWindow.cpp:108`). Рецепт живёт в двух местах (§5).
2. **Асимметрия slope-guard — закрыта**: slope зашит в `treeMap_`, крона читает тот же источник (§4.4). Случай «плавающая крона» исключён структурно + тест §7.7.
3. **Три версии предиката — закрыта**: один `treeScore`, localmax сравнивает его же (§2.1).
4. **SurfaceHeights — поднят в обязательные** (§4.6): FBM-ноды file-local, дублирование формулы = drift-риск для стволов и вертикальных границ.
5. **Water-guard — удалён** (мёртвый код: воды нет в террейне) + тест §7.9 удалён; вода — в фазу 2.
6. **Константы — заданы** (§4.7), `SEED_TREES` отдельный оффсет от руд.
7. **«Проверить руками» → static_assert** (§3). ID подтверждён: `0x5802`/`0x5803`.
8. **Вертикальная граница — тест §7.3**, требует SurfaceHeights.
9. **Тест «листья не трогают руду» — заменён** на инвариант не-AIR (§7.6).
10. **Мелочи**: убран висячий `- 0`; `BLOCK_LOG/BLOCK_LEAVES` — `constexpr` (`ItemId::pack`); перф: 2592 FBM-сэмпла (2×1296) + ~32k сравнений score; с SurfaceHeights пересчёта 32×32 нет.

**Вопросы ревьюера → ответы**:
- **Q1 (два места рецепта)**: да, подтверждаю кодом. yaml = сервер-авторити, kRecipes = клиентский превью-гейт (кнопка). Паттерн позиционно идентичен (§5).
- **Q2 (density × localmax)**: да, эффективная плотность ~1 ствол на 5×5 в плотном пятне — ожидаемое поведение, «стена леса» недостижима по дизайну. Окно 5×5 не уменьшаем: оно диктуется радиусом кроны (2), меньше — кроны слипаются. Пятна усиливаются `TREE_SCORE_THRESHOLD` (внутри пятна порог ниже → localmax выигрывает чаще).
- **Q3 (slope без типа блока)**: приемлемо. Песка в террейне нет (только STONE/DIRT/GRASS, §1.1) → «деревья на песке» невозможны; slope режет крутые обрывы. Честный `blockAt` — в фазу 2 и требует переделки GenerationQueue (не «лёгкая», §9).
- **Q4 (вода)**: убираю guard и тест. Воды в террейне нет (высоты [32,96], заливка при worldY<0). Уровень воды — отдельная задача (§9).

---

## 11. Чек-лист перед кодом

- [ ] `SurfaceHeights` — общий модуль формулы высот (перенести ноды из anonymous namespace WorldGenerator.cpp:36-43, переписать GenerateTerrain на него)
- [ ] Единый предикат: `treeScore` (chance × density) + localmax 5×5 + slopeOK в `treeMap_` — единственный источник для стволов и крон
- [ ] Предрасчёт `hmap_[36×36]` + `scoreMap_[36×36]` + `treeMap_[36×36]` (2592 FBM + ~32k сравнений)
- [ ] `TreeGenerator` stateless: `const m_seed_`, thread_local буферы, объект на стеке
- [ ] Константы §4.7: `SEED_TREES` (отдельный от руд!), `TREE_SCORE_THRESHOLD`, `SLOPE_MAX`, `MAX_TREE_H`, `FOREST_SCALE`, `BLOCK_LOG/BLOCK_LEAVES` constexpr
- [ ] Рецепт в ДВУХ местах: yaml (§5.1) + `kRecipes` (§5.2), позиционно идентичный паттерн
- [ ] `static_assert` id (§3): `0x5802`/`0x5803`, нет коллизий
- [ ] `GetBlockColor`: log/leaves + фикс dirt/grass (§6)
- [ ] Тесты §7 (вкл. вертикальную границу §7.3, инвариант не-AIR §7.6, «нет крон без ствола» §7.7, MatchGrid §7.8)
- [ ] Сброс `./chunkdb` для визуальной проверки
- [ ] OpenSpec proposal (решение v2 №6)
