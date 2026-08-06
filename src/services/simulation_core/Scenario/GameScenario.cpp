#include "Scenario/GameScenario.h"

#include "core_generated.h"

#include <algorithm>
#include <array>

namespace simcore {

const std::vector<GameScenario> &scenarios() {
  static const std::vector<GameScenario> kScenarios = {
      GameScenario{
          .index = 0,
          .name = "Init Game (Vagrant)",
          .targetMode = static_cast<uint8_t>(Protocol::GameMode_SURVIVAL),
          .giveItems = {{22529, 1},  // 0:10:11:1  crafting table (packed)
                        {30723, 1}}, // 0:11110:3 wooden pickaxe (packed)
          .clearFirst = true,
          .questBookEra = 0, // VAGRANT
      },
      // Future scenarios (e.g. "start at steam era", "skip ore") are appended
      // here as new rows — no new code path required.
  };
  return kScenarios;
}

const GameScenario *findScenario(uint8_t index) {
  const auto &all = scenarios();
  for (const auto &sc : all) {
    if (sc.index == index) return &sc;
  }
  return nullptr;
}

bool applyScenario(PlayerInventoryStore &store, const GameScenario &sc,
                   uint64_t player_id) {
  if (sc.clearFirst) {
    std::array<PersistSlot, kInventorySlots> empty{};
    store.setSlots(player_id, empty);
  }
  for (const auto &give : sc.giveItems) {
    if (!store.giveItem(player_id, give.first, give.second)) return false;
  }
  store.setGameMode(player_id, sc.targetMode);
  return true;
}

} // namespace simcore