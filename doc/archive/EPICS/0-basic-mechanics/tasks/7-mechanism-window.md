# TASK: MechanismWindow class (Window Hierarchy)

**Статус**: Todo
**Эпик**: 0-basic-mechanics
**Зависит от**: архитектура готова (ui.md), MachineWindow и WorkbenchWindow созданы

---

## Описание

Создать промежуточный класс `MechanismWindow` в иерархии `BlockAttachedWindow → MachineWindow/WorkbenchWindow`. Вынести общую для машин логику (энергия, прогресс, capabilities) в базовый класс.

---

## Требования

### MechanismWindow (новый класс)
- Наследуется от `BlockAttachedWindow`
- Поля: `energy_`, `maxEnergy_`, `progress_`, `machineType_`
- Virtual `RenderCapabilities()` — рисует энергобар + прогресс-бар + input/output слоты
- Virtual `GetMachineType()` — возвращает `BlockType`

### Изменения в MachineWindow
- Наследовать от `MechanismWindow` вместо прямого `BlockAttachedWindow`
- `RenderCapabilities()` — добавляет свою специфику (например, специфичные слоты)
- Оставить `GetMachineType()` как override

### Изменения в WorkbenchWindow (опционально)
- Верстак может наследовать напрямую от `BlockAttachedWindow` (без энергии/прогресса)
- Или от `MechanismWindow` с заглушками энергии (проще для единообразия)

---

## Файлы

- `UI/Windows/MechanismWindow.h` — класс MechanismWindow
- `UI/Windows/MechanismWindow.cpp` — RenderCapabilities (SlotGrid + ProgressBar)
- `UI/Windows/MachineWindow.h/.cpp` — refactor to extend MechanismWindow

---

## Acceptance Criteria

#### Сценарий: MachineWindow наследует MechanismWindow
1. MachineWindow больше не extends BlockAttachedWindow напрямую
2. MachineWindow переопределяет RenderCapabilities()
3. Энергобар и прогресс работают из базового класса

#### Сценарий: WorkbenchWindow не сломан
1. WorkbenchWindow продолжает работать после рефакторинга
2. 3×3 grid и кнопка Craft на месте
3. RenderSlotGrid для player slots работает
