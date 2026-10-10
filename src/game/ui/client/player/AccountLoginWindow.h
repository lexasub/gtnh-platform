#pragma once

#include <game/ui/client/IUIWindow.h>

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

struct InventoryState;

class AccountLoginWindow final : public IUIWindow {
public:
  using AuthSubmit =
      std::function<bool(const std::string &, const std::string &, bool)>;
  using AuthSuccess =
      std::function<bool(const std::string &, const std::string &)>;
  AccountLoginWindow(AuthSubmit submit, AuthSuccess saveCredentials)
      : submitAuth_(std::move(submit)),
        saveCredentials_(std::move(saveCredentials)) {}

  std::string_view Name() const override { return "AccountLogin"; }
  void Render(InventoryState *playerInv) override;
  bool IsOpen() const override;
  void SetOpen(bool open) override;
  void HandleAuthResult(uint8_t status, const std::string &message);
  bool SubmitCredentials(const std::string &username, const std::string &password,
                         bool registerAccount = false);
  void PrefillUsername(const std::string &username);

private:
  friend struct AccountLoginWindowTestAccess;
  bool submit(bool register_account);

  AuthSubmit submitAuth_;
  AuthSuccess saveCredentials_;
  // Render runs on RenderLib's worker while auth callbacks arrive from GameClient::Poll.
  mutable std::mutex state_mutex_;
  std::array<char, 33> username_{};
  std::array<char, 129> password_{};
  std::string submittedUsername_;
  std::string submittedPassword_;
  std::string message_;
  bool open_ = false;
  bool pending_ = false;
};
