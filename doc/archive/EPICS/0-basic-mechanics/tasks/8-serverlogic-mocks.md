# TASK: ServerLogic — IMechanism interface + Mock machines

**Статус**: Done
**Эпик**: 0-basic-mechanics
**Зависит от**: 7-mechanism-window (иерархия окон)

---

## Описание

Создать client-side моки серверной логики машин, чтобы UI машин мог работать и отображать прогресс/энергию без работающего NetClient. Механизмы тикают локально на клиенте.

---

## Требования

### IMechanism interface
- `GetMachineType() -> BlockType`
- `GetInputSlots() -> vector<ItemStack>&`
- `GetOutputSlots() -> vector<ItemStack>&`
- `GetProgress() -> float` (0.0–1.0)
- `GetEnergy() -> (current, max)`
- `Tick(float dt)` — тикает прогресс, потребляет энергию

### Mock-имплементации
- `MockFurnace` — плавит input → output за N секунд
- `MockMacerator` — дробит input → output за N секунд
- `MockCompressor` — сжимает input → output за N секунд
- `MockCraftingTable` — без прогресса, просто хранит сетку

### Интеграция с UI
- MachineWindow/MechanismWindow хранит `unique_ptr<IMechanism>`
- `Render()` читает прогресс/энергию из IMechanism
- `Tick(dt)` вызывается из GameClient::Update()
- Когда NetClient готов — IMechanism заменяется на сетевой прокси

---

## Файлы

- `src/services/game_client/ServerLogic/IMechanism.h`
- `src/services/game_client/ServerLogic/MockFurnace.h/.cpp`
- `src/services/game_client/ServerLogic/MockMacerator.h/.cpp`
- `src/services/game_client/ServerLogic/MockCompressor.h/.cpp`

---

## Acceptance Criteria

#### Сценарий: MachineWindow показывает прогресс без сети
1. Игрок открывает MachineWindow (ПКМ по furnace)
2. В окне input слот, output слот пуст, прогресс 0%
3. Через 5 секунд прогресс достигает 100%, output слот заполняется
4. Энергобар показывает "магическую" энергию

#### Сценарий: Смена рецепта
1. Игрок кладёт iron ore в input
2. Furnace начинает плавить (прогресс растёт)
3. Игрок забирает iron ore из input
4. Прогресс сбрасывается, output остаётся

## Done

- **IMechanism.h** — interface с GetMachineType(), GetInput/OutputSlots(), GetProgress/GetEnergy(), Tick(dt)
- **MockFurnace/Mockerator/Compressor** — header-only mocks с inline Tick(), BlockType enum, energy initialization
- **MockCraftingTable** — stub без прогресса, хранит сетку
- **MachineWindow/MechanismWindow** — unique_ptr<IMechanism>, Render() читает прогресс/энергию, Tick() вызывается из GameClient::Update()
- **NetClient integration** — stub для замены на сетевой прокси
