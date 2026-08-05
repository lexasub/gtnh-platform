#pragma once

#include "Windows/IUIWindow.h"
#include "quest_lib/QuestTypes.h"
#include <cstdint>
#include <imgui.h>
#include <string>
#include <unordered_map>
#include <vector>

class UIManager;

// QuestBookWindow — the player's quest progression guide.
// Opened with Q key. Shows era tabs, sections, quests, and detail view.
// Data is loaded locally from quests.csv + quest_graph.json.
// Progress is synced with MetaDB via network messages.
class QuestBookWindow : public IUIWindow {
public:
  explicit QuestBookWindow(UIManager *mgr);

  std::string_view Name() const override { return "QuestBook"; }
  void Render(InventoryState *playerInv) override;
  bool OnKeyEvent(int key, int action, int mods) override;
  bool OnMouseClick(int button, int action) override;
  void OnNetworkUpdate(uint8_t msgType, const void *data) override;

  bool IsOpen() const override { return open_; }
  void SetOpen(bool open) override;
  bool WantsMouseCapture() const override { return open_; }
  bool WantsKeyboardCapture() const override { return open_; }

private:
  bool open_ = false;

  // Currently selected era (0-3) and section
  int selectedEra_ = 0;
  int selectedSection_ = 0;
  uint32_t selectedQuestId_ = 0;

  // Quest data (loaded from files)
  struct QuestEntry {
    uint32_t id;
    std::string title;
    std::string description;
    std::string section;
    std::string detectType;
    std::string detectTarget;
    uint16_t rewardItemId;
    uint8_t rewardCount;
    uint16_t costItemId;
    uint8_t costCount;
    uint16_t cooldownSecs;
    bool isExchange;
    uint8_t status; // 0=locked, 1=available, 2=in_progress, 3=completed
    uint8_t progress;
  };

  std::vector<QuestEntry> quests_;
  bool dataLoaded_ = false;

  // Exchange state for the currently selected quest. Cooldown is fetched
  // server-side on each quest detail open; the response arrives async.
  uint32_t cooldownRemainingSecs_ = 0;
  uint32_t cooldownQueriedQuestId_ = 0;
  std::string exchangeMessage_;

  std::vector<quest::EraInfo> eraData_;
  std::vector<bool> eraCompleted_;
  // Pending era index to auto-select on next render (transition may arrive
  // before the window is first drawn).
  int newlyAvailableEra_ = -1;

  void loadQuestData();
  void applyQuestStatus(uint32_t questId, uint8_t status, uint8_t progress);
  void applyEraTransition(uint8_t completedEra, uint8_t nextEra);
  int eraIndexFor(uint8_t eraVal) const;

  // Layout helpers
  void renderEraTabs();
  void renderSectionPanel();
  void renderQuestList();
  void renderQuestDetail(const InventoryState* playerInv);
  void renderCompletionBadge(uint8_t status);
  void onCompleteClicked(uint64_t playerId);
  void onExchangeClicked(uint64_t playerId);
  void onCooldownQuery(uint64_t playerId);
  int countItem(const InventoryState* inv, uint16_t itemId) const;

  // Color helpers
  ImU32 statusColor(uint8_t status) const;
  const char *statusLabel(uint8_t status) const;

  UIManager *uiMgr_ = nullptr;
};