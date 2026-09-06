#include "storage/SecureStorage.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nexus::storage {

namespace fs = std::filesystem;
using core::ErrorCode;
using core::Result;
using core::SecretString;
using core::Status;

namespace {
// Reject keys that could escape the store directory or collide with hidden files.
bool validKey(const std::string& key) {
  if (key.empty()) return false;
  for (char c : key) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) {
      return false;
    }
  }
  return key.front() != '.';
}
}  // namespace

SecureStorage::SecureStorage(std::string dir) : dir_(std::move(dir)) {}

std::string SecureStorage::pathFor(const std::string& key) const { return dir_ + "/" + key; }

Status SecureStorage::put(const std::string& key, const SecretString& value) {
  if (!validKey(key)) return Status::error(ErrorCode::InvalidArg, "invalid secret key");

  std::error_code ec;
  fs::create_directories(dir_, ec);
  ::chmod(dir_.c_str(), 0700);  // ensure the store dir is private

  const std::string target = pathFor(key);
  const std::string tmp = target + ".tmp";

  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return Status::error(ErrorCode::SecureStoreError,
                         std::string("open failed: ") + std::strerror(errno));
  }
  const std::string& bytes = value.reveal();
  const char* p = bytes.data();
  size_t remaining = bytes.size();
  while (remaining > 0) {
    ssize_t n = ::write(fd, p, remaining);
    if (n < 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      return Status::error(ErrorCode::SecureStoreError,
                           std::string("write failed: ") + std::strerror(errno));
    }
    p += n;
    remaining -= static_cast<size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
  if (::rename(tmp.c_str(), target.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return Status::error(ErrorCode::SecureStoreError,
                         std::string("rename failed: ") + std::strerror(errno));
  }
  return Status::success();
}

Result<SecretString> SecureStorage::get(const std::string& key) const {
  if (!validKey(key)) return Status::error(ErrorCode::InvalidArg, "invalid secret key");
  const std::string target = pathFor(key);
  std::ifstream in(target, std::ios::binary);
  if (!in) return Status::error(ErrorCode::NotFound, "secret not found: " + key);
  std::ostringstream ss;
  ss << in.rdbuf();
  return SecretString(ss.str());
}

bool SecureStorage::has(const std::string& key) const {
  return validKey(key) && fs::exists(pathFor(key));
}

Status SecureStorage::erase(const std::string& key) {
  if (!validKey(key)) return Status::error(ErrorCode::InvalidArg, "invalid secret key");
  std::error_code ec;
  fs::remove(pathFor(key), ec);
  return Status::success();
}

}  // namespace nexus::storage
