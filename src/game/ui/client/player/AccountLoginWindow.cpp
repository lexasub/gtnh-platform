#include "AccountLoginWindow.h"

#include <imgui.h>

#include <algorithm>

bool AccountLoginWindow::submit(bool register_account) {
  if (pending_ || !submitAuth_) return false;
  const std::string username(username_.data());
  std::string password(password_.data());
  if (username.empty() || password.empty()) {
    message_ = "Enter both username and password.";
    return false;
  }
  pending_ = submitAuth_(username, password, register_account);
  if (pending_) {
    submittedUsername_ = username;
    submittedPassword_ = password;
  }
  std::fill(password.begin(), password.end(), '\0');
  std::fill(password_.begin(), password_.end(), '\0');
  message_ = pending_ ? "Contacting server..." : "Not connected to the server.";
  return pending_;
}

bool AccountLoginWindow::SubmitCredentials(const std::string &username,
                                           const std::string &password,
                                           bool registerAccount) {
  if (username.size() >= username_.size() || password.size() >= password_.size())
    return false;
  std::lock_guard<std::mutex> lock(state_mutex_);
  std::fill(username_.begin(), username_.end(), '\0');
  std::fill(password_.begin(), password_.end(), '\0');
  std::copy(username.begin(), username.end(), username_.begin());
  std::copy(password.begin(), password.end(), password_.begin());
  open_ = true;
  return submit(registerAccount);
}

void AccountLoginWindow::PrefillUsername(const std::string &username) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  std::fill(username_.begin(), username_.end(), '\0');
  const size_t count = std::min(username.size(), username_.size() - 1);
  std::copy_n(username.begin(), count, username_.begin());
  std::fill(password_.begin(), password_.end(), '\0');
  open_ = true;
}

bool AccountLoginWindow::IsOpen() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return open_;
}

void AccountLoginWindow::SetOpen(bool open) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  open_ = open;
}

void AccountLoginWindow::HandleAuthResult(uint8_t status,
                                          const std::string &message) {
  std::string username;
  std::string password;
  AuthSuccess saveCredentials;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    pending_ = false;
    std::fill(password_.begin(), password_.end(), '\0');
    message_ = message;
    if (status == 0) {
      username = std::move(submittedUsername_);
      password = std::move(submittedPassword_);
      saveCredentials = saveCredentials_;
      message_ = "Signed in. Starting game...";
    } else {
      std::fill(submittedPassword_.begin(), submittedPassword_.end(), '\0');
      submittedPassword_.clear();
      submittedUsername_.clear();
      open_ = true;
    }
  }

  if (status == 0 && saveCredentials && !saveCredentials(username, password)) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    message_ = "Signed in, but could not save local credentials.";
  }
  std::fill(password.begin(), password.end(), '\0');
}

void AccountLoginWindow::Render(InventoryState *) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!open_) return;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(390.0f, 0.0f), ImGuiCond_Always);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove;
  if (!ImGui::Begin("GTNH Account", nullptr, flags)) {
    ImGui::End();
    return;
  }
  ImGui::TextWrapped("Register an account or sign in to restore your player data.");
  ImGui::InputText("Username", username_.data(), username_.size());
  ImGui::InputText("Password", password_.data(), password_.size(),
                   ImGuiInputTextFlags_Password |
                       ImGuiInputTextFlags_EnterReturnsTrue);

  const bool buttonsDisabled = pending_;
  if (buttonsDisabled) ImGui::BeginDisabled();
  if (ImGui::Button("Sign in", ImVec2(170.0f, 0.0f))) submit(false);
  ImGui::SameLine();
  if (ImGui::Button("Register", ImVec2(170.0f, 0.0f))) submit(true);
  if (buttonsDisabled) ImGui::EndDisabled();
  if (!message_.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", message_.c_str());
  }
  ImGui::End();
}
