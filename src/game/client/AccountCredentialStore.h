#pragma once

#include <filesystem>
#include <optional>
#include <string>

struct AccountCredentials {
  std::string username;
  std::string password;
};

class AccountCredentialStore {
public:
  AccountCredentialStore();
  explicit AccountCredentialStore(std::filesystem::path filePath);

  static std::optional<std::filesystem::path> ResolveDefaultPath();

  [[nodiscard]] const std::filesystem::path &FilePath() const { return filePath_; }
  [[nodiscard]] std::optional<AccountCredentials> Load() const;
  [[nodiscard]] bool Save(const AccountCredentials &credentials) const;
  [[nodiscard]] bool Remove() const;

private:
  std::filesystem::path filePath_;
};
