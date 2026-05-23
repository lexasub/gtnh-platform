# TASK: NetClient Wire-Up

**Статус**: Todo (blocked — ждёт API NetClient)
**Эпик**: 0-basic-mechanics

---

## Описание

Подключить UIManager к NetClient, чтобы изменения инвентаря и состояния машин синхронизировались через сеть (FlatBuffers по TCP).

---

## Требования

### InventoryUpdate callback
- `NetClient::SetInventoryUpdateCallback(callback)` — при получении InventoryUpdate
- callback применяет новые слоты в `GameClient::invState_`
- UIManager::DispatchNetwork → InventoryWindow::OnNetworkUpdate

### InventoryAction отправка
- При клике по слоту → `NetClient::SendInventoryAction(slot, item_id, count)`
- Действие уходит через Gateway → SimulationCore

### BlockEntityUpdate
- При ПКМ по машине/верстаку → запрос состояния
- SimulationCore отвечает `BlockEntityUpdate` → Gateway → Client
- UIManager::DispatchNetwork → MachineWindow::OnNetworkUpdate

---

## Acceptance Criteria

#### Сценарий: InventoryUpdate обновляет UI
1. Сервер шлёт InventoryUpdate с 45 слотами
2. Client получает сообщение
3. InventoryWindow отображает новые слоты

#### Сценарий: Клик по слоту шлёт InventoryAction
1. Игрок перемещает предмет в инвентаре
2. `SendInventoryAction` вызван
3. Сообщение уходит через Gateway

#### Сценарий: BlockEntityUpdate для машины
1. Игрок открывает MachineWindow (ПКМ)
2. Клиент запрашивает состояние
3. Сервер шлёт BlockEntityUpdate
4. MachineWindow отображает прогресс/энергию/слоты
