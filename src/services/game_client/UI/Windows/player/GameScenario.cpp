#include "GameScenario.h"

#include "ConsoleWindow.h"
#include "QuestBookWindow.h"
#include "UIManager.h"
#include "Common/Inventory.h"
#include "Network/NetClient.h"
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
    inv->gameMode = static_cast<GameMode>(resp->game_mode());
    spdlog::info("[GameScenario] Applied game mode {} from scenario {}",
                 static_cast<int>(resp->game_mode()),
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
