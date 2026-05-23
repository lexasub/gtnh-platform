# TASK: BlockType enum consolidation

**Статус**: Done
**Эпик**: 0-basic-mechanics

---

## Описание

Сейчас block ID размазаны: magic numbers 10–13 в BlockUIFactory, отдельный enum в FlatBuffers, хардкод строк в механизмах. Создать единый `BlockType` enum в `Common/BlockType.h` для client и server.

---

## Требования

### BlockType enum
- `Common/BlockType.h`: `enum class BlockType : uint16_t { AIR=0, STONE=1, DIRT=2, GRASS=3, ..., WORKBENCH=100, FURNACE=101, MACERATOR=102, COMPRESSOR=103 }`
- В будущем расширяется — добавление не ломает нумерацию

### Где используется
- `BlockUIFactory` — ключи регистрации
- `MachineWindow::GetMachineType()` — возвращает BlockType
- `IMechanism::GetMachineType()` — возвращает BlockType
- FlatBuffers schema — `block_id: uint16` (значение BlockType)
- `UIDefaults::TryOpenBlockUI()` — сравнение с BlockType
- CreativeMenu — список предметов

### Исключения
- Не пытаться мигрировать ChunkStore или SimulationCore на C++ enum — они получают uint16_t по сети
- FlatBuffers schema остаётся uint16_t, не enum (совместимость)

---

## Файлы

- `Common/BlockType.h` — новый файл с enum

---

## Acceptance Criteria

#### Сценарий: Все reference на магические числа заменены
1. Нет чисел 10, 11, 12, 13 в BlockUIFactory, MachineWindow и креатив-меню
2. `BlockUIFactory::RegisterBlock(BlockType::FURNACE, ...)` вместо `RegisterBlock(11, ...)`
3. Код компилируется без ошибок
4. FlatBuffers schema не поменялась

## Done

- **Common/BlockType.h** — enum class BlockType: uint16_t (AIR=0, STONE=1, ..., WORKBENCH=100, FURNACE=101, MACERATOR=102, COMPRESSOR=103)
- **BlockUIFactory** — migrated: RegisterBlock(BlockType::FURNACE, ...) вместо RegisterBlock(11, ...)
- **MachineWindow** — GetMachineType() возвращает BlockType
- **IMechanism** — GetMachineType() возвращает BlockType
- **UIDefaults** — TryOpenBlockUI() сравнивает с BlockType
- **CreativeMenu** — список предметов использует BlockType
- Zero magic numbers (10–13) ликвидированы
