#include "GameScenario.h"

#include "ConsoleWindow.h"
#include "QuestBookWindow.h"
#include "UIManager.h"
#include <game/client/Common/Inventory.h>
#include <apps/game_client/Network/NetClient.h>
#include "core_generated.h"

#include <spdlog/spdlog.h>

GameScenario::GameScenario(UIManager *mgr) : uiMgr_(mgr) {}

void GameScenario::OnNetworkUpdate(uint8_t msgType, const void *data) {
  if (msgType != GatewayMsg::kStartScenarioResp || !data) return;

  auto* uiMgr = uiMgr_;
  if (!uiMgr) return;

  flatbuffers::Verifier v(static_cast<const uint8_t *>(data), 8192);
  if (!v.VerifyBuffer<Protocol::StartScenarioResp>(nullptr)) {
    spdlog::warn("[GameScenario] invalid StartScenarioResp buffer");
    return;
  }
  auto* resp = flatbuffers::GetRoot<Protocol::StartScenarioResp>(
      static_cast<const uint8_t *>(data));
  if (!resp) return;

  auto* console = uiMgr->Find<ConsoleWindow>();
  if (!resp->success()) {
    if (console) {
      console->addOutput("Start scenario failed: " +
                         (resp->error() ? resp->error()->str() : std::string("unknown error")));
    }
    return;
  }

  if (auto* inv = uiMgr->GetPlayerInventory()) {
    // `game_mode` is an unchecked uint8 off the wire (core.fbs:25) and
    // flatbuffers does not range-check enum values on read, so this must be
    // validated before the value becomes the client's mode — otherwise the
    // client runs in a mode the enum does not name. Rejected, not clamped:
    // clamping to 3 would silently hand a bogus 255 the full SPECTATOR
    // permission set (fly, noclip, infinite items). The mode is left
    // untouched so a bad response cannot disturb the mode the player is
    // already in (gp-ul16).
    GameMode mode{};
    if (TryGameModeFromWire(resp->game_mode(), mode)) {
      inv->gameMode = mode;
    } else {
      spdlog::warn(
          "[GameScenario] rejecting out-of-range game_mode {} from scenario "
          "{}, keeping {}",
          static_cast<int>(resp->game_mode()),
          static_cast<int>(resp->scenario_index()),
          GameModeName(inv->gameMode));
    }
    spdlog::info("[GameScenario] Applied game mode {} from scenario {}",
                 static_cast<int>(inv->gameMode),
                 static_cast<int>(resp->scenario_index()));
  }

  const std::string *msg = nullptr;
  for (const auto &sc : gamescenario::scenarios()) {
    if (sc.index == static_cast<uint8_t>(resp->scenario_index())) {
      msg = &sc.outputMessage;
      break;
    }
  }
  if (console) console->addOutput(msg ? *msg : "Scenario completed.");

  if (auto* qb = uiMgr->Find<QuestBookWindow>()) {
    qb->SetOpen(true);
    qb->SetEra(static_cast<int>(resp->quest_book_era()));
  }
  // Authoritative player era — gates recipe visibility (UX filter).
  uiMgr->SetCurrentEra(resp->quest_book_era());
}
