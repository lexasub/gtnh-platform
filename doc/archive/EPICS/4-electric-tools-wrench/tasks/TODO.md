# Tasks: Electric Tools & Machine Config

## A. Electric Tools

- [ ] Item registration: drill_ulv, drill_lv, drill_mv, drill_hv, chainsaw_lv, wrench
- [ ] Battery Buffer block registration (LV, MV, HV)
- [ ] EnergyStorage в meta предмета
- [x] ToolAction FlatBuffers протокол (core.fbs + gateway.fbs) — **DONE 2026-06-27**
- [x] WrenchHandler/ToolAction обработчик в SimulationCore — **DONE 2026-06-27**
- [ ] Mining speed = f(tier, block_hardness)
- [ ] CAS block break + drill energy consumption
- [ ] Battery Buffer tick (20 Hz) — зарядка
- [ ] Client: BatteryBuffer GUI (slot для drill)
- [ ] Client: заряд инвентаря в тултипе

## B. Wrench & Machine Side Config

- [x] side_config: array<uint8, 6> в MachineComponent — **✅ exists**
- [x] ToolAction/ToolActionResp протокол (core.fbs) — **✅ wiring done 2026-06-27**
- [x] Gateway обработка kToolAction → publish "player.tool.action" — **✅ DONE**
- [x] SimulationCore subscribe "player.tool.action" → WrenchHandler::cycleFace → publish response — **✅ DONE**
- [x] Client G key → SendToolAction(WRENCH_CYCLE) → ToolActionRespCallback — **✅ DONE**
- [x] Циклическое переключение ролей (INPUT→OUTPUT→ENERGY→FLUID_IN→FLUID_OUT→ANY→NONE) — **✅ DONE**
- [ ] Client: raycast face detection при ПКМ ключом — **B3 pending**
- [ ] Сохранение side_config в EntityStateStore
- [ ] Publish machine.config.updated
- [ ] Client: обновление текстуры грани
- [ ] PipeNetwork: учёт side_config при BFS routing
