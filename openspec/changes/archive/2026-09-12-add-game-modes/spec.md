# add-game-modes — In-Game Console + Gamemode Switch

**Status**: implemented (client + server wired)
**Plan**: `/home/su/.claude/plans/imperative-exploring-moth.md`

## Summary

In-game developer console (F4) + `/gamemode 0|1|2|3` command with full
client-server protocol. Gamemode gates CreativeMenu, camera flight, and
block interaction on the client. Server stores per-player mode in
SimulationCore and echoes changes back.

## Files

### Client
| File | Status | Purpose |
|------|--------|---------|
| `src/services/game_client/Common/Inventory.h` | modified | `GameMode` enum, `gameMode` field |
| `src/services/game_client/UI/Windows/player/ConsoleWindow.h` | new | Console IUIWindow |
| `src/services/game_client/UI/Windows/player/ConsoleWindow.cpp` | new | Render, commands, history draft preservation, network send |
| `src/services/game_client/UI/Core/ActionHandler.h` | modified | `DoToggleConsole()` |
| `src/services/game_client/UI/Core/ActionHandler.cpp` | modified | `toggle_console` + CreativeMenu gamemode gate |
| `src/services/game_client/UI/Core/InputBinder.cpp` | modified | F4 → `toggle_console` |
| `src/services/game_client/UI/UIDefaults.cpp` | modified | `Register<ConsoleWindow>` |
| `src/services/game_client/UI/Windows/player/CreativeMenu.cpp` | modified | Auto-close on non-CREATIVE |
| `src/services/game_client/GameClient.cpp` | modified | Flight gate + interaction gate per gamemode |
| `src/services/game_client/Camera/Camera.h` | modified | `SetFlightEnabled(bool)` |
| `src/services/game_client/Camera/Camera.cpp` | modified | Vertical movement gate |
| `src/services/game_client/Network/NetClient.h` | modified | `kGameModeChange=30`, callback, `SendGameModeChange()` |
| `src/services/game_client/Network/NetClient.cpp` | modified | OnMessage dispatch + send impl |
| `src/services/game_client/UI/CMakeLists.txt` | modified | ConsoleWindow.cpp |

### Protocol
| File | Status | Purpose |
|------|--------|---------|
| `src/protocol/core.fbs` | modified | `GameMode` enum, `table GameModeChange` |

### Server
| File | Status | Purpose |
|------|--------|---------|
| `src/services/gateway/gateway.h` | modified | `kGameModeChange=30` |
| `src/services/gateway/gateway.cpp` | modified | Route `kGameModeChange` → `"player.gamemode.change"`; relay `"player.gamemode.changed"` → client |
| `src/services/gateway/main.cpp` | modified | `subscribe("player.gamemode.changed")` |
| `src/services/simulation_core/Storage/PlayerInventoryStore.h` | modified | `playerModes_` + `setGameMode()`/`getGameMode()` |
| `src/services/simulation_core/Network/SimCoreMessageHandler.cpp` | modified | Handle `"player.gamemode.change"`: store + echo |
| `src/services/simulation_core/main.cpp` | modified | `Subscribe("player.gamemode.change")` |

## Architecture

```
F4 → InputBinder → ActionHandler::DoToggleConsole() → ConsoleWindow::SetOpen()

/gamemode 1 (in console):
  → inv->gameMode = CREATIVE
  → NetClient::SendGameModeChange(pid, 1)
  → GatewayMsg::kGameModeChange (wire 30) → Gateway
  → publish("player.gamemode.change") → SimulationCore
  → PlayerInventoryStore::setGameMode(pid, mode)
  → publish("player.gamemode.changed") → Gateway
  → send_to_client_ctrl_raw(kGameModeChange) → Client
  → OnMessage → onGameModeChange_(new_mode)
```

## Commands
- `/gamemode 0` → SURVIVAL (no flight, block breaking allowed)
- `/gamemode 1` → CREATIVE (flight, instant break, creative menu)
- `/gamemode 2` → ADVENTURE (no block break/place)
- `/gamemode 3` → SPECTATOR (no interaction)
- `/help` — list commands

## Gamemode Effects (client-side)

| Behavior | SURVIVAL | CREATIVE | ADVENTURE | SPECTATOR |
|----------|----------|----------|-----------|-----------|
| CreativeMenu (Tab) | ❌ | ✅ | ❌ | ❌ |
| Flight (Space/Shift) | ❌ | ✅ | ❌ | ✅ |
| Block break/place | ✅ | ✅ | ❌ | ❌ |
| Camera look | ✅ | ✅ | ✅ | ✅ |
| Camera movement | ✅ | ✅ | ✅ | ✅ |

## ConsoleWindow Design
- ImGui overlay: 70% width, 35% height, bottom-center, 85% alpha
- Output log: last 100 lines, auto-scroll
- Input buffer: 256 chars, `/` prefix visible
- History: last 50 commands, ↑/↓ navigation with draft preservation (`draft_` field)
- Extensible: `RegisterCommand(name, fn, help)`
- Validated input: manual digit parsing (no `atoi` silent errors)

## Review Bugs Fixed
| # | Bug | Fix |
|---|-----|-----|
| 1 | `WantsKeyboardCapture()` unconditional `true` | → `return open_` |
| 2 | `atoi` maps non-numeric to SURVIVAL | → manual digit validation |
| 3 | Unused includes | → removed |

## Remaining Gaps (not yet addressed)

### P0 — Keyboard capture bypass (user: "забей, другой агент")
- `UIManager::ProcessInput` never checks `WantsKeyboardCapture()`. Typing E/R/U/Tab/0-9 in console fires bound actions.
- `SetKeyboardFocusHere()` every frame prevents coexisting UI.
- 256-char buffer silently truncates.

### P2 — UX (user: "забей, другой агент")
- Escape closes ALL windows (DoCloseAll), not just console.
- `/gamemode` with no args shows usage instead of current mode.

### P3 — Persistence
- `InventoryState::gameMode` defaults to `CREATIVE` on every client restart.
- Server stores mode in memory only (`PlayerInventoryStore::playerModes_`) — lost on simcore restart.
- Mode survives reconnect (client re-receives echo from server), but not cold start.
