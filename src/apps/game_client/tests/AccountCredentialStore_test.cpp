#include <game/client/AccountCredentialStore.h>

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct TempDirectory {
  std::filesystem::path path;
  TempDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "gtnh-credentials-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    char *created = ::mkdtemp(name.data());
    if (created) path = created;
  }
  ~TempDirectory() {
    if (!path.empty()) std::filesystem::remove_all(path);
  }
  explicit operator bool() const { return !path.empty(); }
};

bool check(bool condition, const char *message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

bool testConfigPathResolution() {
  TempDirectory temp;
  if (!temp) return check(false, "mkdtemp failed");
  const auto xdg = temp.path / "xdg";
  const auto home = temp.path / "home";
  ::setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
  ::setenv("HOME", home.c_str(), 1);
  auto path = AccountCredentialStore::ResolveDefaultPath();
  if (!check(path && *path == xdg / "gtnh-platform" / "account-credentials.json",
             "absolute XDG_CONFIG_HOME path resolution failed"))
    return false;

  ::setenv("XDG_CONFIG_HOME", "relative/config", 1);
  path = AccountCredentialStore::ResolveDefaultPath();
  if (!check(path && *path == home / ".config" / "gtnh-platform" /
                                "account-credentials.json",
             "relative XDG_CONFIG_HOME must fall back to HOME"))
    return false;

  ::unsetenv("HOME");
  path = AccountCredentialStore::ResolveDefaultPath();
  ::unsetenv("XDG_CONFIG_HOME");
  return check(!path, "missing HOME should disable credential persistence");
}

bool testSaveLoadPermissionsAndReplacement() {
  TempDirectory temp;
  if (!temp) return check(false, "mkdtemp failed");
  const auto file = temp.path / "gtnh-platform" / "account-credentials.json";
  AccountCredentialStore store(file);
  if (!check(store.Save({" Player_ONE ", "correct-horse-battery"}),
             "credential save failed"))
    return false;

  struct stat dirInfo {}, fileInfo {};
  if (!check(::stat(file.parent_path().c_str(), &dirInfo) == 0 &&
                 (dirInfo.st_mode & 0777) == 0700,
             "credential directory is not mode 0700") ||
      !check(::stat(file.c_str(), &fileInfo) == 0 &&
                 (fileInfo.st_mode & 0777) == 0600,
             "credential file is not mode 0600"))
    return false;

  auto loaded = store.Load();
  if (!check(loaded && loaded->username == "player_one" &&
                 loaded->password == "correct-horse-battery",
             "saved credentials did not load in canonical form"))
    return false;

  if (!check(!store.Save({"invalid-name!", "correct-horse-battery"}),
             "invalid username should not overwrite saved credentials"))
    return false;
  loaded = store.Load();
  if (!check(loaded && loaded->username == "player_one" &&
                 loaded->password == "correct-horse-battery",
             "failed save damaged previous credentials"))
    return false;

  if (!check(store.Save({"second_user", "replacement-password"}),
             "atomic replacement failed"))
    return false;
  loaded = store.Load();
  if (!check(loaded && loaded->username == "second_user" &&
                 loaded->password == "replacement-password",
             "replacement credentials were not loaded"))
    return false;
  if (!check(store.Remove() && !store.Load(), "credential removal failed"))
    return false;
  return true;
}

bool testRejectsUnsafeAndMalformedFiles() {
  TempDirectory temp;
  if (!temp) return check(false, "mkdtemp failed");
  const auto dir = temp.path / "gtnh-platform";
  std::filesystem::create_directories(dir);
  ::chmod(dir.c_str(), 0700);
  const auto file = dir / "account-credentials.json";
  AccountCredentialStore store(file);

  {
    std::ofstream out(file);
    out << "not json";
  }
  ::chmod(file.c_str(), 0600);
  if (!check(!store.Load(), "malformed JSON should be rejected")) return false;

  std::filesystem::remove(file);
  const auto target = temp.path / "target.json";
  {
    std::ofstream out(target);
    out << R"({"version":1,"username":"valid_user","password":"valid-password"})";
  }
  ::chmod(target.c_str(), 0600);
  std::filesystem::create_symlink(target, file);
  if (!check(!store.Load(), "symlink credential file should be rejected")) return false;
  if (!check(!store.Save({"valid_user", "replacement-password"}),
             "unsafe destination should not be overwritten"))
    return false;

  std::filesystem::remove(file);
  {
    std::ofstream out(file);
    out << R"({"version":1,"username":"valid_user","password":"valid-password"})";
  }
  ::chmod(file.c_str(), 0644);
  if (!check(!store.Load(), "group/world-readable credential file should be rejected"))
    return false;

  std::filesystem::remove(file);
  {
    std::ofstream out(file);
    out << std::string(4097, 'x');
  }
  ::chmod(file.c_str(), 0600);
  return check(!store.Load(), "oversized credential file should be rejected");
}
} // namespace

int main() {
  if (!testConfigPathResolution()) return 1;
  if (!testSaveLoadPermissionsAndReplacement()) return 1;
  if (!testRejectsUnsafeAndMalformedFiles()) return 1;
  return 0;
}
