# Игрок и крафт (Player & Crafting)

**Эпик**: Gameplay — Player & Crafting
**Слой**: Layer 1 (Basic Mechanics)
**Статус**: ✅ Implemented (end-to-end pipeline working)

## Affected Services

| Service | Layer | Role |
|---------|-------|------|
| **MetaDB** (Go/SQLite) | L0 | Primary — player inventory persistence, position saves |
| **SimulationCore** | L1 | Post-hook — subscribes to inventory/craft events for gameplay effects |
| **RecipeManager** ⬅️ **NEW** | L0 | Dependency — validates craftable recipes |
| **Gateway** | L0 | Relay — forwards PlayerAction/InventoryUpdate |
| **GameClient** | L1 | Consumer — inventory GUI, crafting UI, creative menu |
| **EntityStateStore** ⬅️ **NEW** | L0 | Persistence — workbench inventory state |

> **Architecture rule**: Inventory is owned by MetaDB. SimulationCore does not manage inventory state — it hooks into events (post-hooks) for gameplay logic.



---

## Связь с EPIC «Basic Mechanics — MVP»

Описание базовых механик игрока (инвентарь, установка/ломка блоков, верстак, креативное меню, правила MVP) **вынесено** в отдельный EPIC:

> 📄 [0-basic-mechanics/basic-mechanics.md](../0-basic-mechanics/basic-mechanics.md)

Оттуда:
- **Секция 1** — Инвентарь игрока (хотбар, полный инвентарь, открытие/закрытие, персистентность)
- **Секция 2** — Взаимодействие с блоками (place/break, нет инструментов, нет дропа)
- **Секция 3** — Машины и верстак (CRAFTING_TABLE, сетка 3×3, stub крафта)
- **Секция 4** — Креативное меню
- **Секция 6** — Базовые правила MVP
- **Секция 7** — Протокол (PlayerAction, InventoryUpdate, CraftRequest, BlockEntityUpdate)

Данный EPIC содержит архитектурные детали реализации инвентарей и крафта.

---

## Архитектура

### MetaDB

Хранит `player_inventory` (blob). ✅ router_client.go подключён к MessageRouter (heartbeat, PublishInventoryUpdate). Осталось: таблицы players + player.logout.

### SimulationCore

✅ CraftRequestHandler — подписан на `sim.craft.request`, запускает GridPatternMatcher + RecipeManager, публикует результат.

### RecipeManager

✅ Загружен в SimulationCore (embedded, не отдельный сервис). 6 JSON-файлов рецептов. ItemRegistry подключён для name↔ID конвертации.

### EntityStateStore

✅ Отдельный C++ сервис (entitystated). LMDB-бэкенд, TCP RPC порт 5200, MessageRouter pub/sub на `entity.state.get/set`.

---

## Протокол (дополнения к basic-mechanics.md)

```flatbuffers
// CraftRequest — подробно: вызов RecipeManager
table CraftRequest {
    player_id: uint64;
    x: uint32; y: uint32; z: uint32;
    slots: [ItemStack];
}
```

**Важно**: крафт только через блок верстака. Крафта в инвентаре (2×2) не будет.

## Реализация

### Data Flow (работает)
```
Workbench [Craft] → NetClient::SendCraftRequest
  → io_uring(type 9) → Gateway::client_ctrl_read_cb
  → publish("sim.craft.request") → CraftRequestHandler
  → GridPatternMatcher (shape-aware 3×3) + RecipeManager (13 recipes)
  → publish("sim.craft.response") → Gateway
  → NetClient::OnMessage(type 10)
```

### Компоненты SimulationCore
| Компонент | Файлы | Статус |
|-----------|-------|--------|
| GridPatternMatcher | `Crafting/GridPatternMatcher.h/.cpp` | ✅ shape-aware, empty slot semantics |
| CraftRequestHandler | `Crafting/CraftRequestHandler.h/.cpp` | ✅ sim.craft.request/response sub/pub |
| WorkbenchStateManager | `Crafting/WorkbenchStateManager.h/.cpp` | ✅ in-memory grid state |
| CraftInventoryFlow | `Crafting/CraftInventoryFlow.h/.cpp` | ✅ consumption/production delta |
| RecipeManager | `RecipeManager/RecipeManager.cpp` | ✅ ItemRegistry::nameToId(), 6 recipe JSONs |
| ConditionEvaluator | `RecipeManager/ConditionEvaluator.cpp` | ✅ 152-line real impl (called with empty MachineState) |

### Компоненты GameClient
| Компонент | Статус |
|-----------|--------|
| NetClient::SendCraftRequest() | ✅ io_uring type 9 |
| NetClient::OnMessage type 10 | ✅ CraftResponse handler |
| WorkbenchWindow Craft button | ✅ wired via UIManager::SetNetClient() |
| MatchPattern() preview | ✅ 8 embedded patterns |

### Осталось (на момент архивации)
- ~~CraftResponse UI feedback~~ ✅ DONE — цветной текст с таймером (ClientCraftingWindow.cpp:20-38, 91-104)
- ~~Server-authoritative grid state через TileEntityStore RPC~~ → перенесено в `0-foundation-item-inventory`
- ~~Полный inventory consumption через MetaDB~~ → перенесено в `0-foundation-item-inventory`
- ~~ConditionEvaluator с реальным MachineState~~ → перенесено в `1-gameplay-machines-multiblocks`

---

## Открытые вопросы (дополнительные)
