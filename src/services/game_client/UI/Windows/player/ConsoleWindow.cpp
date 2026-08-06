#include "ConsoleWindow.h"

#include "UIManager.h"
#include "Common/Inventory.h"
#include "Network/NetClient.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

// ──────────────────────────────────────────────────────────────────────────
// Construction
// ──────────────────────────────────────────────────────────────────────────

ConsoleWindow::ConsoleWindow(UIManager *mgr) : uiMgr_(mgr) {
  // Register built-in commands
  RegisterCommand("help", [](ConsoleWindow *cw, InventoryState *,
                             const std::vector<std::string_view> &) {
    cw->addOutput("=== Commands ===");
    cw->addOutput("  /gamemode <0|1|2|3>");
    cw->addOutput("    0 = survival, 1 = creative, 2 = adventure, 3 = spectator");
    cw->addOutput("  /help  — show this message");
  }, "Show available commands");

  RegisterCommand("gamemode",
      [](ConsoleWindow *cw, InventoryState *inv,
         const std::vector<std::string_view> &args) {
        if (args.empty()) {
          cw->addOutput("Usage: /gamemode <0|1|2|3>");
          cw->addOutput("  0 = survival, 1 = creative, 2 = adventure, 3 = spectator");
          return;
        }
        // Strict numeric validation: reject non-numeric, empty, or trailing garbage
        int mode = 0;
        {
          const std::string& s = std::string(args[0]);
          if (s.empty()) {
            cw->addOutput("Usage: /gamemode <0|1|2|3>");
            return;
          }
          for (char ch : s) {
            if (ch < '0' || ch > '9') {
              cw->addOutput("Invalid gamemode: expected integer 0-3, got \"" + s + "\"");
              return;
            }
            mode = mode * 10 + (ch - '0');
            if (mode > 3) {
              cw->addOutput("Invalid gamemode. Use 0 (survival), 1 (creative), 2 (adventure), or 3 (spectator)");
              return;
            }
          }
        }
        if (inv) {
          inv->gameMode = static_cast<GameMode>(mode);
          cw->addOutput(std::string("Game mode set to ") + GameModeName(inv->gameMode));
          spdlog::info("[Console] Gamemode switched to {} ({})",
                       static_cast<int>(mode), GameModeName(inv->gameMode));
          if (auto *nc = cw->uiMgr_->GetNetClient())
            nc->SendGameModeChange(inv->player_id, static_cast<uint8_t>(mode));
        }
      },
      "Switch game mode: 0=survival, 1=creative, 2=adventure, 3=spectator");
}

// ──────────────────────────────────────────────────────────────────────────
// Command registration
// ──────────────────────────────────────────────────────────────────────────

void ConsoleWindow::RegisterCommand(const std::string &name, CommandFn fn,
                                    const std::string &help) {
  commands_[name] = std::move(fn);
  if (!help.empty()) {
    commandHelp_[name] = help;
  }
}

// ──────────────────────────────────────────────────────────────────────────
// Open / Close
// ──────────────────────────────────────────────────────────────────────────

void ConsoleWindow::SetOpen(bool open) {
  open_ = open;
  if (open) {
    reclaimFocus_ = true; // refocus input on next frame
    historyPos_ = -1;
    inputBuf_[0] = '\0';
    draft_.clear();
  }
}

// ──────────────────────────────────────────────────────────────────────────
// Command execution
// ──────────────────────────────────────────────────────────────────────────

void ConsoleWindow::executeCommand(const std::string &line, InventoryState *playerInv) {
  // Echo the command
  addOutput("> " + line);

  // Add to history
  if (!line.empty()) {
    history_.push_back(line);
    if (history_.size() > kMaxHistory) {
      history_.pop_front();
    }
  }
  historyPos_ = -1;

  // Parse: skip leading '/'
  std::string_view trimmed = line;
  if (!trimmed.empty() && trimmed[0] == '/') {
    trimmed = trimmed.substr(1);
  }

  // Split into command + args
  std::string cmd;
  std::vector<std::string_view> args;

  auto spacePos = trimmed.find(' ');
  if (spacePos != std::string_view::npos) {
    cmd = std::string(trimmed.substr(0, spacePos));
    // Parse remaining args (split by space)
    std::string_view rest = trimmed.substr(spacePos + 1);
    while (!rest.empty()) {
      // Skip leading spaces
      auto start = rest.find_first_not_of(' ');
      if (start == std::string_view::npos) break;
      rest = rest.substr(start);
      auto end = rest.find(' ');
      if (end == std::string_view::npos) {
        args.push_back(rest);
        break;
      }
      args.push_back(rest.substr(0, end));
      rest = rest.substr(end + 1);
    }
  } else {
    cmd = std::string(trimmed);
  }

  if (cmd.empty()) return;

  auto it = commands_.find(cmd);
  if (it != commands_.end()) {
    it->second(this, playerInv, args);
  } else {
    addOutput(std::string("Unknown command: ") + cmd + " (type /help for commands)");
  }
}

void ConsoleWindow::addOutput(const std::string &msg) {
  outputLog_.push_back(msg);
  if (outputLog_.size() > kMaxOutputLines) {
    outputLog_.pop_front();
  }
}

// ──────────────────────────────────────────────────────────────────────────
// Input handling (arrow keys for history, escape handled by InputBinder)
// ──────────────────────────────────────────────────────────────────────────

bool ConsoleWindow::OnKeyEvent(int key, int action, int mods) {
  if (!open_) return false;
  // We handle key events via ImGui's input text — GLFW callbacks for
  // special keys that ImGui doesn't capture well (arrows in text input).
  (void)key;
  (void)action;
  (void)mods;
  return false; // let ImGui handle everything
}

// ──────────────────────────────────────────────────────────────────────────
// Render
// ──────────────────────────────────────────────────────────────────────────

void ConsoleWindow::Render(InventoryState *playerInv) {
  if (!open_) return;

  // ── Focus management ───────────────────────────────────────────────────
  if (reclaimFocus_) {
    ImGui::SetNextWindowFocus();
    reclaimFocus_ = false;
  }

  // ── Window setup ───────────────────────────────────────────────────────
  const auto &io = ImGui::GetIO();
  ImVec2 winSize(io.DisplaySize.x * 0.7f, io.DisplaySize.y * 0.35f);
  ImVec2 winPos(io.DisplaySize.x * 0.15f, io.DisplaySize.y * 0.60f);

  ImGui::SetNextWindowPos(winPos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(winSize, ImGuiCond_Always);

  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings;

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.85f);
  ImGui::Begin("##console", nullptr, flags);

  // ── Output log ─────────────────────────────────────────────────────────
  float footerHeight =
      ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing() + 4.0f;
  ImGui::BeginChild("##output", ImVec2(0, -footerHeight), true);
  ImGui::PushTextWrapPos();
  for (const auto &line : outputLog_) {
    ImGui::TextUnformatted(line.c_str());
  }
  // Auto-scroll to bottom
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
    ImGui::SetScrollHereY(1.0f);
  }
  ImGui::PopTextWrapPos();
  ImGui::EndChild();

  // ── Input line ─────────────────────────────────────────────────────────
  ImGui::PushItemWidth(-1); // full width
  ImGui::TextUnformatted("/");
  ImGui::SameLine();
  // Hide label by using ## prefix
  ImGui::SetKeyboardFocusHere(); // auto-focus each frame
  bool enterPressed = ImGui::InputText("##cmdinput", inputBuf_, sizeof(inputBuf_),
                                       ImGuiInputTextFlags_EnterReturnsTrue |
                                           ImGuiInputTextFlags_AutoSelectAll);
  ImGui::PopItemWidth();

  // ── Handle Enter ───────────────────────────────────────────────────────
  if (enterPressed && inputBuf_[0] != '\0') {
    executeCommand(inputBuf_, playerInv);
    inputBuf_[0] = '\0';
    draft_.clear();
    reclaimFocus_ = true;
  }

  // ── History navigation (up/down while input is focused) ───────────────
  if (ImGui::IsItemFocused()) {
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !history_.empty()) {
      if (historyPos_ == -1) {
        draft_.assign(inputBuf_); // preserve unsent draft
        historyPos_ = static_cast<int>(history_.size()) - 1;
      } else if (historyPos_ > 0) {
        historyPos_--;
      }
      auto &entry = history_[historyPos_];
      std::strncpy(inputBuf_, entry.c_str(), sizeof(inputBuf_) - 1);
      inputBuf_[sizeof(inputBuf_) - 1] = '\0';
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
      if (historyPos_ != -1) {
        if (historyPos_ < static_cast<int>(history_.size()) - 1) {
          historyPos_++;
          auto &entry = history_[historyPos_];
          std::strncpy(inputBuf_, entry.c_str(), sizeof(inputBuf_) - 1);
          inputBuf_[sizeof(inputBuf_) - 1] = '\0';
        } else {
          historyPos_ = -1; // back to draft
          std::strncpy(inputBuf_, draft_.c_str(), sizeof(inputBuf_) - 1);
          inputBuf_[sizeof(inputBuf_) - 1] = '\0';
        }
      }
      // historyPos_ == -1: not navigating, keep current input
    }
  }

  ImGui::End();
  ImGui::PopStyleVar();
}
