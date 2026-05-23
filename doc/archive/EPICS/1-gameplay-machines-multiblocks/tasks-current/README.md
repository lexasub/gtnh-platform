# Tasks: 1-gameplay-machines-multiblocks — L1 Completion

## Фаза 1: Machine Slot Interaction (SetMachineSlotResp)

| # | Файл | Что делать | Зависит от | Приоритет |
|---|------|-----------|-----------|-----------|
| 01 | `01-set-machine-slot-resp-protocol.md` | Add `SetMachineSlotResp` table to `core.fbs` + GatewayMsg enum | — | 🔴 High |
| 02 | `02-set-machine-slot-resp-simcore.md` | SimulationCore шлёт SetMachineSlotResp после обработки | 01 | 🔴 High |
| 03 | `03-set-machine-slot-resp-client.md` | Клиент обрабатывает SetMachineSlotResp (UI фидбек) | 01, 02 | 🔴 High |

**Обоснование**: Machine slot interaction УЖЕ РАБОТАЕТ (SetMachineSlotReq есть, Gateway публикует, SimulationCore обрабатывает, MachineWindow отправляет). Нужен только SetMachineSlotResp для явного ACK.

## Фаза 2: Server Machine Registration

| # | Файл | Что делать | Зависит от | Приоритет |
|---|------|-----------|-----------|-----------|
| 04 | `04-fix-ismachineblock.md` | `isMachineBlock()` → `MachineRegistry::getMachineInfo()` | — | 🔴 High |
| 05 | `05-fix-defaultslotcount.md` | `defaultMachineSlotCount()` → registry lookup | 04 | 🟡 Medium |
| 06 | `06-fix-energystorage-init.md` | EnergyStorage init из MachineRegistry (capacity, tier) | 04 | 🟡 Medium |
| 07 | `07-complete-machine-registry.md` | Все 13 машин в CSV с корректными параметрами | — | 🟡 Medium |
| 08 | `08-validation-registered-only.md` | Только registered machines получают MachineComponent | 04 | 🔴 High |
| 09 | `09-ess-save.md` | Machine entity → EntityStateStore при создании | 08 | 🔵 Low |
| 10 | `10-ess-remove.md` | Machine entity → ESS remove при destruction | 08, 09 | 🔵 Low |

## Порядок выполнения

```
Phase 1 (quick win)
  01 → 02 → 03   (~2-3 часа, SetMachineSlotResp)

Phase 2 (architecture cleanup)
  04 → 05 → 06 → 07 → 08 → 09 → 10   (~1-2 дня)
```

## Правила (из анализа кода)

1. **Ноль хардкода** — `isMachineBlock()`, `defaultMachineSlotCount()`, список block_id в ChunkEventHandler — всё через `MachineRegistry::getMachineInfo()` или `MachineRegistry::All()`
2. **SetMachineSlotReq уже существует** — задача 01-03 только добавляют RESPONSE, request уже реализован
3. **MachineRegistry — единый источник истины** — все проверки "это машина?" через registry, не через хардкод
4. **EntityStateStore (C++ LMDB) уже работает** — не надо реализовывать, только подключить для новых машин

## Архивировано (DONE / STALE / L2)

См. `_archive/`:
- `1-machine-blocks-recipes.md` — STALE (block_id 101-103 не совпадают с реальностью 36-63)
- `2-machine-ecs-tick.md` — ✅ DONE (MachineSystem 3-pass tick)
- `3-machine-client-gui.md` — ✅ DONE (MachineWindow)
- `4-blockentityupdate-protocol.md` — ✅ DONE (FlatBuffer)
- `5-entity-state-store.md` — STALE (описан SQLite MVP, реально C++ LMDB)
- `6-multiblock-foundation.md` — L2 DEFERRED (→ doc/EPICS/7-multiblocks-l2/)
- `7-layer2-machine-state-dataflow.md` — ✅ DONE (ConditionEvaluator работает)
