#pragma once

#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../IUIWindow.h"

class UIManager;
struct InventoryState;

// ──────────────────────────────────────────────────────────────────────────
// ConsoleWindow — in-game command console (F4)
//
// Opens as a bottom-half overlay. Supports:
//   /gamemode 0|1|2|3  — switch game mode (survival/creative/adventure/spectator)
//   /help               — list available commands
//
// Commands are extensible via RegisterCommand().
// Game mode is stored in InventoryState::gameMode (client-authoritative).
// ──────────────────────────────────────────────────────────────────────────
class ConsoleWindow : public IUIWindow {
public:
  explicit ConsoleWindow(UIManager *mgr);

  std::string_view Name() const override { return "Console"; }

  void Render(InventoryState *playerInv) override;
  bool OnKeyEvent(int key, int action, int mods) override;

  bool IsOpen() const override { return open_; }
  void SetOpen(bool open) override;

  bool WantsMouseCapture() const override { return open_; }
  bool WantsKeyboardCapture() const override { return open_; }

  // ── Command registration ──────────────────────────────────────────────
  using CommandFn = std::function<void(ConsoleWindow *, InventoryState *,
                                       const std::vector<std::string_view> &)>;
  void RegisterCommand(const std::string &name, CommandFn fn,
                       const std::string &help = "");

private:
  void executeCommand(const std::string &line, InventoryState *playerInv);
  void addOutput(const std::string &msg);

  UIManager *uiMgr_ = nullptr;
  bool open_ = false;

  // Input buffer
  char inputBuf_[256] = "";

  // Command history
  std::deque<std::string> history_;
  int historyPos_ = -1; // -1 = current input, 0..N = history index (0=oldest)
  std::string draft_;   // unsent input preserved while navigating history

  // Output log
  std::deque<std::string> outputLog_;
  static constexpr size_t kMaxOutputLines = 100;
  static constexpr size_t kMaxHistory = 50;

  // Registered commands
  std::unordered_map<std::string, CommandFn> commands_;
  std::unordered_map<std::string, std::string> commandHelp_;

  // Focus management
  bool reclaimFocus_ = false;
};
