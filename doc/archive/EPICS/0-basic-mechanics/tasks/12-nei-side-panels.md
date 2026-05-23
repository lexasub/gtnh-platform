# TASK: NEI-style Side Panels (Future)

**Статус**: Done (Foundation)
**Эпик**: 0-basic-mechanics
**Зависит от**: MechanismWindow (task 7), drag-and-drop (task 10)

---

## Описание

Создать систему контекстных панелей, которые отображаются рядом с открытым MachineWindow/MechanismWindow — аналогично NEI/JEI в Java-версии GTNH.

---

## Требования

### ISidePanel interface
```cpp
class ISidePanel {
    virtual const char* GetPanelName() = 0;
    virtual void Render(int windowX, int windowY, int windowW) = 0;
    virtual bool IsVisibleFor(IUIWindow* window) = 0;
};
```

### Виды панелей
- **RecipePanel** — показывает рецепты для текущей машины
- **InfoPanel** — информация о блоке/предмете
- **HistoryPanel** — история крафтов

### Интеграция
- UIManager владеет списком ISidePanel
- При рендеринге MachineWindow: UIManager проверяет `IsVisibleFor()` и рендерит панель справа
- Панель переиспользуется между окнами одного типа

---

## Acceptance Criteria

#### Сценарий: RecipePanel рядом с MachineWindow
1. Игрок открывает MachineWindow (furnace)
2. Справа от окна появляется RecipePanel
3. В панели показаны рецепты плавки (iron ore → iron ingot)
4. При закрытии окна панель исчезает

#### Сценарий: Разные панели для разных машин
1. Furnace показывает рецепты плавки
2. Macerator показывает рецепты дробления
3. Compressor показывает рецепты сжатия

## Done (Foundation)

- **ISidePanel.h** — interface: GetPanelName(), Render(windowX/windowY/windowW), IsVisibleFor(IUIWindow*)
- **RecipePanel.h** — stub, показывает рецепты для текущей машины
- **UIManager** — panels registry, RenderPanels() вызывает Render() для каждой панели
- **UIDefaults** — регистрация ISidePanel при открытии MachineWindow
- **Panel caching** — переиспользование панелей между окнами одного типа
- Visibility wiring deferred: MachineWindow → UIManager → RenderPanels
