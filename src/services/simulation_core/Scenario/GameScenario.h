// GameScenario.h — server-side scenario table for "start game" flow.
//
// A scenario is DATA, not code: index → (name, target mode, items to grant,
// clear-first flag, quest-book era). The client sends only the index
// (StartScenarioReq); SimulationCore owns the contents and applies them
// server-authoritatively via PlayerInventoryStore.
#pragma once

#include "Storage/PlayerInventoryStore.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace simcore {

/// One playable start scenario. Scenario 0 = initial survival kit.
struct GameScenario {
  uint8_t index = 0;
  std::string name;                                       // display name
  uint8_t targetMode = 0;                                 // Protocol::GameMode (0 = SURVIVAL)
  std::vector<std::pair<uint16_t, uint8_t>> giveItems;    // (packed_item_id, count)
  bool clearFirst = false;                                // scenario 0: wipe inventory first
  uint8_t questBookEra = 0;                               // 0 = VAGRANT
};

/// Static scenario table, ordered by index. Lookup is O(n) over a tiny list.
const std::vector<GameScenario> &scenarios();

/// Returns the scenario with the given index, or nullptr if out of range.
const GameScenario *findScenario(uint8_t index);

/// Applies a scenario to a player's inventory store:
///   clearFirst → setSlots(empty) → giveItem × N → setGameMode(targetMode).
/// Each store call fires its onChange/postMutation callbacks synchronously, so
/// `player.inventory.update` snapshots are enqueued before the caller publishes
/// the scenario response. Returns true when every step succeeded.
bool applyScenario(PlayerInventoryStore &store, const GameScenario &sc,
                   uint64_t player_id);

} // namespace simcore
