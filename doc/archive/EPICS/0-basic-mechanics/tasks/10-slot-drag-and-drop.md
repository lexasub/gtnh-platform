# TASK: Drag-and-Drop в SlotGrid

**Статус**: Todo
**Эпик**: 0-basic-mechanics
**Зависит от**: SlotGrid готов

---

## Описание

Добавить drag-and-drop между слотами в SlotGridComponent. Клик → взять предмет, клик → положить, Shift+клик → быстрое перемещение между инвентарями.

---

## Требования

### Базовая механика (Simple Click)
- ЛКМ по слоту с предметом → предмет "взят" (курсор держит)
- ЛКМ по пустому слоту → предмет положен
- ЛКМ по слоту с предметом (когда уже держишь) → swap

### Shift+Click
- Shift+ЛКМ по слоту → предмет перемещается между инвентарями (player ↔ machine)
- Если machine inventory полон → не перемещается

### Drag ввод/вывод
- `SlotGridComponent` отслеживает hover/click state
- Callback: `OnSlotClick(int slot, int button, bool shift)`
- Интеграция с InventoryState + MechanismWindow

---

## Файлы

- `UI/Components/SlotGrid.h/.cpp` — add drag state, callbacks
- `UI/Windows/InventoryWindow.h/.cpp` — wire callbacks to inventory state
- `UI/Windows/MachineWindow.h/.cpp` — wire между machine и player инвентарями

---

## Acceptance Criteria

#### Сценарий: Переместить предмет в инвентаре
1. Игрок открывает инвентарь (E)
2. Кликает по слоту с камнем
3. Камень "прилипает" к курсору
4. Кликает по пустому слоту
5. Камень перемещается в новый слот

#### Сценарий: Swap предметов
1. В инвентаре слот A = камень, слот B = земля
2. Клик по A → курсор держит камень
3. Клик по B → камень в B, земля в A

#### Сценарий: Shift+Click в машину
1. Открыт MachineWindow
2. Shift+клик по углю в инвентаре игрока
3. Уголь перемещается в input слот машины
4. Если input занят — ничего не происходит
