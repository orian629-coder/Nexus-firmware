#include "updater/Installer.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

namespace nexus::updater {

namespace fs = std::filesystem;
using core::ErrorCode;
using core::Status;

Status Installer::install(const std::vector<std::uint8_t>& payload) {
  if (payload.empty()) return Status::error(ErrorCode::InvalidArg, "empty payload");

  const std::string tmp = target_ + ".new";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd < 0) {
    return Status::error(ErrorCode::IoError, std::string("open failed: ") + std::strerror(errno));
  }
  const auto* p = reinterpret_cast<const char*>(payload.data());
  std::size_t remaining = payload.size();
  while (remaining > 0) {
    ssize_t n = ::write(fd, p, remaining);
    if (n < 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      return Status::error(ErrorCode::IoError, std::string("write failed: ") + std::strerror(errno));
    }
    p += n;
    remaining -= static_cast<std::size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);

  if (::rename(tmp.c_str(), target_.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return Status::error(ErrorCode::IoError, std::string("rename failed: ") + std::strerror(errno));
  }
  ::chmod(target_.c_str(), 0755);
  return Status::success();
}

}  // namespace nexus::updater
