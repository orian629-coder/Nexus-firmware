#include "calibration/CalibrationProfiles.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace nexus::calibration {

namespace fs = std::filesystem;
using core::ErrorCode;
using core::Result;
using core::Status;

namespace {
bool validName(const std::string& n) {
  if (n.empty() || n.front() == '.') return false;
  for (char c : n) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
  }
  return true;
}
}  // namespace

CalibrationProfiles::CalibrationProfiles(std::string dir) : dir_(std::move(dir)) {}

std::string CalibrationProfiles::pathFor(const std::string& name) const {
  return dir_ + "/" + name + ".json";
}

Status CalibrationProfiles::save(const CalibrationProfile& profile) {
  if (!validName(profile.name)) return Status::error(ErrorCode::InvalidArg, "invalid profile name");
  std::error_code ec;
  fs::create_directories(dir_, ec);

  nlohmann::json j = {{"name", profile.name},
                      {"score", profile.score},
                      {"room_noise_dbfs", profile.room_noise_dbfs},
                      {"eq_gains", profile.eq_gains}};

  const std::string target = pathFor(profile.name);
  const std::string tmp = target + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return Status::error(ErrorCode::IoError, std::strerror(errno));
  const std::string data = j.dump(2);
  if (::write(fd, data.data(), data.size()) != static_cast<ssize_t>(data.size())) {
    ::close(fd);
    ::unlink(tmp.c_str());
    return Status::error(ErrorCode::IoError, "short write");
  }
  ::fsync(fd);
  ::close(fd);
  if (::rename(tmp.c_str(), target.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return Status::error(ErrorCode::IoError, "rename failed");
  }
  return Status::success();
}

Result<CalibrationProfile> CalibrationProfiles::load(const std::string& name) const {
  if (!validName(name)) return Status::error(ErrorCode::InvalidArg, "invalid profile name");
  std::ifstream in(pathFor(name));
  if (!in) return Status::error(ErrorCode::NotFound, "profile not found: " + name);
  try {
    nlohmann::json j;
    in >> j;
    CalibrationProfile p;
    p.name = j.value("name", name);
    p.score = j.value("score", 0.0);
    p.room_noise_dbfs = j.value("room_noise_dbfs", 0.0);
    auto gains = j.at("eq_gains").get<std::vector<double>>();
    for (int i = 0; i < dsp::kEqBands && i < static_cast<int>(gains.size()); ++i)
      p.eq_gains[static_cast<std::size_t>(i)] = gains[static_cast<std::size_t>(i)];
    return p;
  } catch (const std::exception& e) {
    return Status::error(ErrorCode::Corrupt, e.what());
  }
}

bool CalibrationProfiles::has(const std::string& name) const {
  return validName(name) && fs::exists(pathFor(name));
}

}  // namespace nexus::calibration
