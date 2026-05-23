# UI Architecture — Current State & Remaining Work

**Среда**: `src/services/game_client/`
**Рендер**: bgfx + ImGui
**Паттерны**: Mediator (UIManager), Strategy (IUIWindow), Factory (BlockUIFactory), Observer (network dispatch)

---

## 1. TL;DR (для субагентов)

- **GameClient знает НОЛЬ типов окон.** Он вызывает `UIDefaults::RegisterPlayerUI()` при старте и `UIDefaults::TryOpenBlockUI()` при клике. Всё.
- **UIManager (Mediator)** владеет всеми окнами, диспатчит input/render/network.
- **BlockUIFactory** создаёт окна лениво (FindOrCreate) по blockId при первом ПКМ.
- **UIDefaults — единственный .cpp**, который включает хедеры конкретных окон. Добавить новое окно = одна регистрация здесь.
- **ImGui-рендер** идёт через `IUIWindow::Render()`, вызывается `UIManager::RenderAll()`.
- **RenderBridge** имеет статический `g_uiMgr*` — ImGui overlay callback не может быть методом класса.

---

## 2. Directory Structure

```
src/services/game_client/
├── Common/
│   └── Inventory.h                # ItemStack + InventoryState (shared data types)
├── ServerLogic/                   # Client-side machine simulation
│   └── IMechanism.h               # Interface: slots, progress, energy, Tick
├── UI/
│   ├── Core/
│   │   ├── IUIWindow.h            # Base interface (Render, input, network, IsOpen)
│   │   └── BlockAttachedWindow.h  # Intermediate: adds BlockPos, IsBlockAttached
│   ├── Windows/
│   │   ├── InventoryWindow.h/.cpp # Hotbar 10 + inventory 4×10, E-key toggle
│   │   ├── CreativeMenu.h/.cpp    # Tab key, search + spawn items
│   │   ├── WorkbenchWindow.h/.cpp # 3×3 grid, extends BlockAttachedWindow
│   │   └── MachineWindow.h/.cpp   # Data-driven: IMechanism*, SetMechanism/GetMechanism/Tick
│   ├── Components/
│   │   └── SlotGrid.h/.cpp        # SlotStyle, RenderSlot, RenderSlotGrid, RenderHotbar, ClickCallback
│   ├── UIManager.h/.cpp           # Mediator: owns windows + panels, dispatch input/render/network
│   ├── BlockUIFactory.h/.cpp      # Factory: registry blockId→Creator, FindOrCreate
│   └── UIDefaults.h/.cpp          # Wire-up: RegisterPlayerUI, TryOpenBlockUI — ONLY file with concrete includes
├── Render/
│   └── RenderBridge.h/.cpp        # ImGui overlay, holds g_uiMgr
└── GameClient.h/.cpp              # Main loop — only includes UIDefaults.h, no window headers
```

RenderLib/UI/ImGuiBackend и RenderLib/UI/Minimap — render-layer компоненты. НЕ трогать.

---

## 3. Class Hierarchy

```
IUIWindow                          # Base (Render, OnKeyEvent, OnMouseClick, OnNetworkUpdate)
  │
  ├── InventoryWindow              # Player inventory — no block binding
  ├── CreativeMenu                 # Creative item spawner — no block binding
  │
  └── BlockAttachedWindow          # Adds BlockPos, IsBlockAttached, GetBlockPos
        │
        ├── MachineWindow          # Data-driven via IMechanism* — slots, progress, energy
        ├── WorkbenchWindow        # 3×3 crafting grid (no energy/progress)
        └── (future: ChestWindow, TankWindow...)

# Data layer (not UI hierarchy)
IMechanism                         # Interface: GetMachineType, GetInputSlots, GetOutputSlots,
  │                                #           GetProgress, GetEnergy, Tick(dt)
  ├── MockFurnace                  # Client-side mock, cookTime=5s
  ├── MockMacerator                # Client-side mock, cookTime=8s
  └── MockCompressor               # Client-side mock, cookTime=10s
```

```
IUIWindow interface:
  Render(InventoryState* playerInv) — ImGui render each frame
  OnKeyEvent(int key, int action)    — keyboard input
  OnMouseClick(int button, int x, int y) — mouse input (ImGui-native preferred)
  OnNetworkUpdate(const uint8_t* data, size_t len) — FlatBuffers update
  IsOpen() / SetOpen(bool)          — visibility
  Name()                            — unique window identifier
```

---

## 4. What's Built (current state)

| Компонент | Статус | Что делает |
|-----------|--------|-----------|
| IUIWindow.h | ✅ Done | Базовый интерфейс, pure virtual |
| BlockAttachedWindow.h | ✅ Done | +BlockPos, IsBlockAttached |
| UIManager.h/.cpp | ✅ Done | OpenExclusive, CloseAll, Find\<T\>(), RenderAll/ProcessInput/DispatchNetwork |
| BlockUIFactory.h/.cpp | ✅ Done | Registry blockId→Creator lambda, FindOrCreate, CanOpen |
| UIDefaults.h/.cpp | ✅ Done | RegisterPlayerUI, TryOpenBlockUI |
| SlotGrid.h/.cpp | ✅ Done | RenderSlot, RenderSlotGrid, RenderHotbar, SlotGridComponent, ClickCallback |
| InventoryWindow | ✅ Done | IUIWindow, hotbar+4×10, E-key toggle |
| CreativeMenu | ✅ Done | IUIWindow, Tab key, search+browse, spawn stub |
| WorkbenchWindow | ✅ Done | BlockAttachedWindow, 3×3 grid |
| MachineWindow | ✅ Done | BlockAttachedWindow + IMechanism* — data-driven slots/progress/energy |
| IMechanism.h | ✅ Done | Interface: GetMachineType, slots, progress, energy, Tick |
| MockFurnace/Macerator/Compressor | ✅ Done | Header-only client-side mocks, Tick() simulates processing |
| Common/Inventory.h | ✅ Done | ItemStack + InventoryState |
| GameClient.h/.cpp | ✅ Done | Init → RegisterPlayerUI, Update → TryOpenBlockUI |
| RenderBridge | ✅ Done | g_uiMgr → UIManager::RenderAll |

---

## 5. Architectural Boundaries (НЕ НАРУШАТЬ)

```
                    GameClient
                        │
                        ▼
              ┌─────────────────┐
              │   UIDefaults    │  ← единственный файл, знающий типы окон
              └────────┬────────┘
                       │
              ┌────────▼────────┐
              │   UIManager     │  ← Mediator, не знает типы окон, только IUIWindow*
              └────────┬────────┘
                       │
         ┌─────────────┼─────────────┐
         ▼             ▼             ▼
  BlockUIFactory   IUIWindow*[]   RenderBridge
  (blockId→window) (Registry)    (g_uiMgr static)
```

**Границы:**
1. **GameClient не включает ни один хедер конкретного окна.** Включения только: `UIManager.h`, `UIDefaults.h`, `BlockUIFactory.h`, `Inventory.h`.
2. **UIDefaults — единственный .cpp**, который делает `#include "UI/Windows/InventoryWindow.h"`. Нигде больше.
3. **UIManager хранит окна как `std::vector<std::unique_ptr<IUIWindow>>`.** Никаких dynamic_cast для типов (кроме `Find<T>()` — только для штатной навигации).
4. **BlockUIFactory создаёт окна лениво (FindOrCreate)** — не пре-регистрирует, не pre-alloc. Первый ПКМ → factory создаёт, повторный → переиспользует.
5. **RenderBridge::ImGuiOverlay** — C-style callback, поэтому `g_uiMgr` статический. Это ок, но нельзя забывать его занулять при shutdown.
6. **Window-файлы в `UI/Windows/`.** `UI/Core/` — только интерфейсы. `UI/Components/` — только переиспользуемые виджеты.
7. **Никаких `FrameRenderData.ext` полей для UI.** Данные UI не пропихиваются через render pipeline. Network update → UIManager::DispatchNetwork → IUIWindow::OnNetworkUpdate.

---

## 6. Key Decisions

| Решение | Почему |
|---------|--------|
| **FindOrCreate вместо pre-registration** | Блок-окна создаются при первом клике. Экономит память, не нужно чистить при открытии. |
| **UIDefaults — единственная точка включения** | Добавление окна не меняет GameClient/UIManager. 1 registration + 1 factory lambda = done. |
| **ImGui::IsKeyPressed вместо InputState** | InputState::keys[] хранит held-state. ImGui даёт press-once семантику, не нужно следить за флагами. |
| **Static g_uiMgr в RenderBridge** | ImGui overlay callback — C-style функция, нельзя передать this. Статик — simplest bridge. |
| **BlockUIFactory — registry function pointers, не virtual** | Блоки — server-side. UI — client-side. Регистрация через лямбды, не virtual methods на Block. |
| **MachineWindow data-driven через IMechanism** | Слоты читаются из `GetInputSlots()`/`GetOutputSlots()` — сколько вернул вектор, столько рисуется. Нет захардкоженных `input_`/`output_`. |
| **IMechanism — клиентские моки вместо сетевого ожидания** | UI должен работать без NetClient. Моки тикают локально. Когда сеть готова — IMechanism заменяется на сетевой прокси. |
| **Нет промежуточного MechanismWindow** | MachineWindow напрямую extends BlockAttachedWindow + IMechanism*. WorkbenchWindow отдельно под BlockAttachedWindow. Без лишних слоёв. |

---

## 7. Anti-Patterns (НЕ ДЕЛАТЬ)

- ❌ **Не включать хедеры окон в GameClient** — весь смысл рефакторинга теряется.
- ❌ **Не хранить FrameExt-структуру** — старый подход, данные UI не через RenderPipeline.
- ❌ **Не делать dynamic_cast цепочку** для определения типа окна. Используйте `Name()` или `Find<T>()`.
- ❌ **Не писать ImGui-код в RenderBridge** кроме `UIManager::RenderAll()`. Всё в IUIWindow::Render().
- ❌ **Не трогать `RenderLib/UI/ImGuiBackend` и `RenderLib/UI/Minimap`** — это render-layer компоненты.
- ❌ **Не хранить Slot-ы с экранными координатами** — ImGui рассчитывает layout автоматически.
- ❌ **Не игнорировать `BlockAttachedWindow`** — если окно привязано к блоку в мире, наследуйтесь от него.
- ❌ **Не пре-регистрировать окна в Init()** — FindOrCreate лениво.
- ❌ **Не создавать промежуточный `MechanismWindow` класс** — MachineWindow напрямую extends BlockAttachedWindow и получает данные через IMechanism*.
- ❌ **Не хардкодить слоты (`input_`/`output_`) в MachineWindow** — слоты data-driven из IMechanism.

---

## 8. Remaining Work (что нужно реализовать)

### 8.1 WorkbenchWindow: слать player slots через Render() параметр (Low)
- Сейчас `WorkbenchWindow` принимает `std::vector<ItemStack>&` в конструкторе — это костыль
- `Render(InventoryState* playerInv)` должен быть источником данных
- Или удалить `playerSlots_` и читать из `playerInv`

### 8.2 ISidePanel система (Medium)
- ✅ ISidePanel.h — интерфейс создан
- ✅ RecipePanel.h — стаб панель
- ✅ UIManager: panels_ registry + RenderPanels
- ✅ UIDefaults: регистрация RecipePanel
- **Остаётся**: привязать видимость панели к открытию MachineWindow (next batch)

### 8.3 BlockType enum consolidation (Low)
- ✅ Common/BlockType.h — enum создан
- ✅ BlockUIFactory переведён на BlockType
- ✅ MachineWindow::GetMachineType() → BlockType
- ✅ IMechanism::GetMachineType() → BlockType

### 8.4 SlotGrid Drag-and-Drop (Medium)
- ✅ ClickCallback добавлен в SlotGridComponent
- **Остаётся**: стейт-машина drag в InventoryWindow/MachineWindow
- Поддержка: клик → взять предмет, клик → положить, Shift+клик → быстрое перемещение

### 8.5 NetClient wire-up (When API Ready)
- `NetClient::SetInventoryUpdateCallback()` — когда API готов
- `NetClient::SendInventoryAction()` — для перемещения предметов
- `NetClient::SendBlockEntityUpdate()` — для машин

### 8.6 UIManager::ProcessInput cleanup (Low)
- Сейчас игнорирует `InputState&` параметр (использует ImGui queries)
- Или удалить параметр, или подключить для non-ImGui input (gamepad и т.д.)

### 8.7 BlockUIFactory::All() рефакторинг (Low)
- FindOrCreateMachine итерирует `UIManager::All()` — это единственная причина почему All() публичен
- Вынести в приватный метод `UIManager::FindWindow(Predicate)` или `FindByType<T>()`

---

## 9. Subagent Implementation Guide

### Как добавить новое окно:
```cpp
// 1. Создать класс, наследующий IUIWindow (или BlockAttachedWindow)
// UI/Windows/MyNewWindow.h
class MyNewWindow : public IUIWindow { ... };

// 2. Зарегистрировать в UIDefaults.cpp (единственное место)
// UIDefaults.cpp
#include "UI/Windows/MyNewWindow.h"
void UIDefaults::RegisterPlayerUI(UIManager& mgr) {
    mgr.Register<MyNewWindow>(...);
}

// 3. Если окно = блок, добавить factory lambda
// BlockUIFactory.cpp
reg[BlockType::MyMachine] = [](UIManager& mgr, BlockPos pos) -> IUIWindow* {
    return FindOrCreate<MyMachineWindow>(mgr, pos, ...);
};
```

### Как добавить новую машину (IMechanism):
```cpp
// 1. Создать клиентский мок
// ServerLogic/MockMyMachine.h
class MockMyMachine : public IMechanism { ... };

// 2. В MachineWindow установить механизм
// MachineWindow::Render() уже читает данные из mech_->GetInputSlots() и т.д.
// SetMechanism() вызывается из BlockUIFactory или при network update
```

### Правила для субагентов:
- Не менять GameClient.h/.cpp (кроме добавления новой регистрации в Init через UIDefaults)
- Не трогать RenderBridge.cpp кроме вызова RenderAll()
- Новые окна → в UI/Windows/
- Новые компоненты → в UI/Components/
- Новые интерфейсы → в UI/Core/
- Новые моки машин → в ServerLogic/
