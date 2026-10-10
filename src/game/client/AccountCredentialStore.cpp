#include "AccountCredentialStore.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr size_t kMaxCredentialFileBytes = 4096;
constexpr mode_t kPrivateDirectoryMode = 0700;
constexpr mode_t kPrivateFileMode = 0600;

bool canonicalizeUsername(const std::string &input, std::string &canonical) {
  size_t begin = 0;
  size_t end = input.size();
  while (begin < end && (input[begin] == ' ' || input[begin] == '\t')) ++begin;
  while (end > begin && (input[end - 1] == ' ' || input[end - 1] == '\t')) --end;
  if (end - begin < 3 || end - begin > 32) return false;

  canonical.clear();
  canonical.reserve(end - begin);
  for (size_t i = begin; i < end; ++i) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      return false;
    canonical.push_back(static_cast<char>(c));
  }
  return true;
}

bool validateCredentials(const AccountCredentials &credentials,
                         std::string &canonicalUsername) {
  return canonicalizeUsername(credentials.username, canonicalUsername) &&
         credentials.password.size() >= 8 && credentials.password.size() <= 128;
}

bool isOwnedPrivateRegularFile(const struct stat &info) {
  return S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
         (info.st_mode & 0077) == 0;
}

bool isPrivateOwnedDirectory(const std::filesystem::path &path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
         info.st_uid == ::geteuid() &&
         (info.st_mode & 0777) == kPrivateDirectoryMode;
}

bool ensurePrivateOwnedDirectory(const std::filesystem::path &path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
      info.st_uid != ::geteuid())
    return false;
  if ((info.st_mode & 0777) != kPrivateDirectoryMode &&
      ::chmod(path.c_str(), kPrivateDirectoryMode) != 0)
    return false;
  return isPrivateOwnedDirectory(path);
}

bool writeAll(int fd, const char *data, size_t size) {
  size_t written = 0;
  while (written < size) {
    const ssize_t n = ::write(fd, data + written, size - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    written += static_cast<size_t>(n);
  }
  return true;
}

class TemporaryFile {
public:
  explicit TemporaryFile(std::string path) : path_(std::move(path)) {}
  ~TemporaryFile() {
    if (active_) ::unlink(path_.c_str());
  }
  const std::string &Path() const { return path_; }
  void Release() { active_ = false; }

private:
  std::string path_;
  bool active_ = true;
};
} // namespace

AccountCredentialStore::AccountCredentialStore() {
  if (auto path = ResolveDefaultPath()) filePath_ = std::move(*path);
}

AccountCredentialStore::AccountCredentialStore(std::filesystem::path filePath)
    : filePath_(std::move(filePath)) {}

std::optional<std::filesystem::path> AccountCredentialStore::ResolveDefaultPath() {
  if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
    const std::filesystem::path base(xdg);
    if (base.is_absolute())
      return base / "gtnh-platform" / "account-credentials.json";
  }
  const char *home = std::getenv("HOME");
  if (!home || !*home) return std::nullopt;
  return std::filesystem::path(home) / ".config" / "gtnh-platform" /
         "account-credentials.json";
}

std::optional<AccountCredentials> AccountCredentialStore::Load() const {
  if (filePath_.empty() ||
      !isPrivateOwnedDirectory(filePath_.parent_path()))
    return std::nullopt;
  struct stat linkInfo {};
  if (::lstat(filePath_.c_str(), &linkInfo) != 0 ||
      !isOwnedPrivateRegularFile(linkInfo) ||
      linkInfo.st_size <= 0 ||
      static_cast<uint64_t>(linkInfo.st_size) > kMaxCredentialFileBytes)
    return std::nullopt;

  const int fd = ::open(filePath_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return std::nullopt;
  struct CloseFd {
    int fd;
    ~CloseFd() { ::close(fd); }
  } closeFd{fd};
  struct stat fileInfo {};
  if (::fstat(fd, &fileInfo) != 0 || !isOwnedPrivateRegularFile(fileInfo) ||
      fileInfo.st_dev != linkInfo.st_dev || fileInfo.st_ino != linkInfo.st_ino ||
      fileInfo.st_size <= 0 ||
      static_cast<uint64_t>(fileInfo.st_size) > kMaxCredentialFileBytes)
    return std::nullopt;

  std::string contents;
  contents.reserve(static_cast<size_t>(fileInfo.st_size));
  std::array<char, 512> buffer{};
  while (contents.size() <= kMaxCredentialFileBytes) {
    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return std::nullopt;
    if (n == 0) break;
    contents.append(buffer.data(), static_cast<size_t>(n));
  }
  if (contents.empty() || contents.size() > kMaxCredentialFileBytes)
    return std::nullopt;

  try {
    const auto document = nlohmann::json::parse(contents);
    if (!document.is_object() || document.size() != 3 ||
        !document.contains("version") || !document.contains("username") ||
        !document.contains("password") || document.at("version") != 1 ||
        !document.at("username").is_string() ||
        !document.at("password").is_string())
      return std::nullopt;
    AccountCredentials credentials{document.at("username").get<std::string>(),
                                   document.at("password").get<std::string>()};
    std::string canonical;
    if (!validateCredentials(credentials, canonical) ||
        canonical != credentials.username)
      return std::nullopt;
    return credentials;
  } catch (const nlohmann::json::exception &) {
    return std::nullopt;
  }
}

bool AccountCredentialStore::Save(const AccountCredentials &credentials) const {
  if (filePath_.empty() || filePath_.filename().empty()) return false;
  std::string canonicalUsername;
  if (!validateCredentials(credentials, canonicalUsername)) return false;

  const auto directory = filePath_.parent_path();
  if (directory.empty()) return false;
  std::error_code fsError;
  std::filesystem::create_directories(directory, fsError);
  if (fsError || !ensurePrivateOwnedDirectory(directory)) return false;

  struct stat existingInfo {};
  if (::lstat(filePath_.c_str(), &existingInfo) == 0) {
    if (!S_ISREG(existingInfo.st_mode) || existingInfo.st_uid != ::geteuid())
      return false;
  } else if (errno != ENOENT) {
    return false;
  }

  nlohmann::json document = {{"version", 1},
                             {"username", canonicalUsername},
                             {"password", credentials.password}};
  std::string serialized = document.dump();
  if (serialized.empty() || serialized.size() > kMaxCredentialFileBytes) return false;

  std::string pattern = (directory / ".account-credentials.json.tmp.XXXXXX").string();
  std::vector<char> temporaryName(pattern.begin(), pattern.end());
  temporaryName.push_back('\0');
  const int fd = ::mkstemp(temporaryName.data());
  if (fd < 0) return false;
  TemporaryFile temporary(temporaryName.data());
  if (::fchmod(fd, kPrivateFileMode) != 0 ||
      !writeAll(fd, serialized.data(), serialized.size()) || ::fsync(fd) != 0) {
    ::close(fd);
    return false;
  }
  if (::close(fd) != 0) return false;
  if (::rename(temporary.Path().c_str(), filePath_.c_str()) != 0) return false;
  temporary.Release();

  const int dirFd = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (dirFd < 0) return false;
  const bool synced = ::fsync(dirFd) == 0;
  ::close(dirFd);
  return synced;
}

bool AccountCredentialStore::Remove() const {
  if (filePath_.empty()) return true;
  struct stat info {};
  if (::lstat(filePath_.c_str(), &info) != 0)
    return errno == ENOENT;
  if (!isOwnedPrivateRegularFile(info)) return false;
  if (::unlink(filePath_.c_str()) != 0) return false;
  const int dirFd = ::open(filePath_.parent_path().c_str(),
                           O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (dirFd < 0) return false;
  const bool synced = ::fsync(dirFd) == 0;
  ::close(dirFd);
  return synced;
}
