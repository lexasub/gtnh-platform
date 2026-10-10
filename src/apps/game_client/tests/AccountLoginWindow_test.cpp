#include <game/client/AccountCredentialStore.h>
#include <game/ui/client/UIManager.h>
#include <game/ui/client/player/AccountLoginWindow.h>
#include <Network/NetClient.h>
#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

void NetClient::SendInventoryAction(uint64_t, uint8_t, uint8_t, uint8_t, uint8_t,
                                    uint16_t, uint8_t) {}

struct AccountLoginWindowTestAccess {
  static void SetCredentials(AccountLoginWindow &window, const char *username,
                             const char *password) {
    std::strncpy(window.username_.data(), username, window.username_.size() - 1);
    std::strncpy(window.password_.data(), password, window.password_.size() - 1);
  }
};

static void renderFrame(AccountLoginWindow &login) {
  ImGui::NewFrame();
  login.Render(nullptr);
  ImGui::Render();
}

int main() {
  char tempTemplate[] = "/tmp/gtnh-login-window-XXXXXX";
  char *tempDir = ::mkdtemp(tempTemplate);
  if (!tempDir) {
    std::fprintf(stderr, "failed to create test directory\n");
    return 1;
  }
  const auto credentialFile = std::filesystem::path(tempDir) / "gtnh-platform" /
                              "account-credentials.json";
  AccountCredentialStore store(credentialFile);

  ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280.0f, 720.0f);
  io.DeltaTime = 1.0f / 60.0f;
  io.Fonts->Build();

  int submitted = 0;
  int saved = 0;
  UIManager ui;
  auto &login = ui.Register<AccountLoginWindow>(
      [&submitted](const std::string &, const std::string &, bool) {
        ++submitted;
        return true;
      },
      [&saved, &store](const std::string &username,
                       const std::string &password) {
        ++saved;
        return store.Save({username, password});
      });
  ui.OpenExclusive(&login);
  if (!login.IsOpen() || !ui.AnyOpen()) {
    std::fprintf(stderr, "AccountLoginWindow is not open after startup registration\n");
    return 1;
  }
  AccountLoginWindowTestAccess::SetCredentials(login, "debuguser",
                                               "fake-password-123");

  // Click Sign in; the fake sender succeeds immediately, exercising the
  // pending_ transition during Render and success-only credential persistence.
  renderFrame(login);
  renderFrame(login);
  renderFrame(login);
  io.AddMousePosEvent(540.0f, 407.0f);
  io.AddMouseButtonEvent(0, true);
  renderFrame(login);
  io.AddMouseButtonEvent(0, false);
  renderFrame(login);

  if (submitted != 1) {
    std::fprintf(stderr, "sign-in click submitted %d requests, want 1\n", submitted);
    return 1;
  }
  login.HandleAuthResult(0, "Authenticated.");
  if (saved != 1) {
    std::fprintf(stderr, "successful auth did not save credentials\n");
    return 1;
  }
  auto stored = store.Load();
  if (!stored || stored->username != "debuguser" ||
      stored->password != "fake-password-123") {
    std::fprintf(stderr, "saved credential file did not round-trip\n");
    return 1;
  }

  if (!login.SubmitCredentials("retry_user", "retry-password-123", false)) {
    std::fprintf(stderr, "manual retry submission failed\n");
    return 1;
  }
  login.HandleAuthResult(1, "Invalid username or password.");
  stored = store.Load();
  if (submitted != 2 || saved != 1 || !stored ||
      stored->username != "debuguser" || stored->password != "fake-password-123") {
    std::fprintf(stderr, "failed auth overwrote saved credentials\n");
    return 1;
  }

  // Simulate next-launch auto-login: load the file and submit a login, not a
  // registration request, without showing or re-entering the password form.
  bool autoLoginWasLogin = false;
  AccountLoginWindow autoLogin(
      [&autoLoginWasLogin](const std::string &, const std::string &, bool isRegistration) {
        autoLoginWasLogin = !isRegistration;
        return true;
      },
      [](const std::string &, const std::string &) { return true; });
  if (!autoLogin.SubmitCredentials(stored->username, stored->password, false) ||
      !autoLoginWasLogin) {
    std::fprintf(stderr, "saved credentials did not submit an automatic login\n");
    return 1;
  }
  autoLogin.HandleAuthResult(1, "Invalid username or password.");
  if (!autoLogin.IsOpen()) {
    std::fprintf(stderr, "rejected auto-login did not return to manual form\n");
    return 1;
  }

  ImGui::DestroyContext();
  std::filesystem::remove_all(tempDir);
  return 0;
}
