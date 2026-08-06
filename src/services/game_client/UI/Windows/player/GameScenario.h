#pragma once

#include "Windows/IUIWindow.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class UIManager;
struct InventoryState;

namespace gamescenario {

/// Display-only mirror of the server's scenario table.
/// Authoritative contents live in SimulationCore (Scenario/GameScenario);
/// this copy is used for /help output and console messages only.
struct ScenarioInfo {
  uint8_t index;
  std::string name;
  uint8_t questBookEra; // 0 = VAGRANT
  std::string outputMessage;
};

/// Client-side scenario table (index-ordered). Empty index → no scenarios.
inline const std::vector<ScenarioInfo> &scenarios() {
  static const std::vector<ScenarioInfo> kScenarios = {
      ScenarioInfo{
          .index = 0,
          .name = "Init Game (Vagrant)",
          .questBookEra = 0,
          .outputMessage =
              "Scenario 0: inventory cleared, starter kit granted "
              "(crafting table + wooden pickaxe), mode SURVIVAL.",
      },
  };
  return kScenarios;
}

/// Strict numeric validation for the /startGameScenario console command.
/// Fills `index` only when `arg` parses to a scenario present in `scenarios()`.
inline bool parseScenarioIndex(std::string_view arg, uint8_t &index) {
  if (arg.empty()) return false;
  uint32_t value = 0;
  for (char ch : arg) {
    if (ch < '0' || ch > '9') return false;
    value = value * 10 + static_cast<uint32_t>(ch - '0');
    if (value > 255) return false;
  }
  const auto candidate = static_cast<uint8_t>(value);
  for (const auto &sc : scenarios()) {
    if (sc.index == candidate) {
      index = candidate;
      return true;
    }
  }
  return false;
}

} // namespace gamescenario

/// GameScenario — invisible window that applies the StartScenarioResp on the
/// client: sets InventoryState::gameMode, prints the scenario output message
/// to the console, and opens the quest book on the era from the response.
/// Never renders anything; registered so UIManager::HandleNetwork reaches it.
class GameScenario : public IUIWindow {
public:
  explicit GameScenario(UIManager *mgr);

  std::string_view Name() const override { return "GameScenario"; }
  void Render(InventoryState *) override {} // display-only, never draws
  bool IsOpen() const override { return false; }
  void SetOpen(bool) override {}
  void OnNetworkUpdate(uint8_t msgType, const void *data) override;

private:
  UIManager *uiMgr_ = nullptr;
};
