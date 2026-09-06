#include "core/AtomicFile.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

namespace nexus::core {

namespace fs = std::filesystem;

Status atomicWriteFile(const std::string& path, const std::string& contents) {
  fs::path target(path);
  fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
  std::error_code ec;
  fs::create_directories(dir, ec);  // best effort

  fs::path tmp = target;
  tmp += ".tmp";

  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return Status::error(ErrorCode::ConfigWriteFailed,
                         std::string("open tmp failed: ") + std::strerror(errno));
  }
  const char* p = contents.data();
  size_t remaining = contents.size();
  while (remaining > 0) {
    ssize_t n = ::write(fd, p, remaining);
    if (n < 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      return Status::error(ErrorCode::ConfigWriteFailed,
                           std::string("write failed: ") + std::strerror(errno));
    }
    p += n;
    remaining -= static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    ::unlink(tmp.c_str());
    return Status::error(ErrorCode::ConfigWriteFailed,
                         std::string("fsync failed: ") + std::strerror(errno));
  }
  ::close(fd);

  if (::rename(tmp.c_str(), target.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return Status::error(ErrorCode::ConfigWriteFailed,
                         std::string("rename failed: ") + std::strerror(errno));
  }

  // Durability of the rename: fsync the containing directory.
  int dfd = ::open(dir.c_str(), O_RDONLY);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return Status::success();
}

}  // namespace nexus::core
