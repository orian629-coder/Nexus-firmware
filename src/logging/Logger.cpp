#include "logging/Logger.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <vector>

namespace nexus::logging {

namespace {
spdlog::level::level_enum toSpd(Level l) {
  switch (l) {
    case Level::Debug: return spdlog::level::debug;
    case Level::Info: return spdlog::level::info;
    case Level::Warning: return spdlog::level::warn;
    case Level::Error: return spdlog::level::err;
    case Level::Critical: return spdlog::level::critical;
  }
  return spdlog::level::info;
}
}  // namespace

Level levelFromString(std::string_view s) {
  if (s == "debug") return Level::Debug;
  if (s == "info") return Level::Info;
  if (s == "warning" || s == "warn") return Level::Warning;
  if (s == "error") return Level::Error;
  if (s == "critical" || s == "crit") return Level::Critical;
  return Level::Info;
}

const char* toString(Level l) {
  switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warning: return "WARNING";
    case Level::Error: return "ERROR";
    case Level::Critical: return "CRITICAL";
  }
  return "INFO";
}

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

core::Status Logger::init(const LogConfig& config) {
  if (impl_) return core::Status::success();  // idempotent

  std::vector<spdlog::sink_ptr> sinks;
  try {
    if (config.to_file) {
      std::filesystem::path p(config.file_path);
      if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        // If the directory can't be created (e.g. running as an unprivileged dev user),
        // fall back to stderr-only rather than failing hard.
        if (ec) {
          sinks.clear();
        } else {
          sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
              config.file_path, config.max_file_bytes, config.max_files));
        }
      }
    }
    if (config.to_stderr || sinks.empty()) {
      sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    }
  } catch (const std::exception& e) {
    return core::Status::error(core::ErrorCode::LogInitFailed, e.what());
  }

  impl_ = std::make_shared<spdlog::logger>("nexus", sinks.begin(), sinks.end());
  impl_->set_pattern("[%Y-%m-%dT%H:%M:%S.%e] [%^%l%$] %v");
  impl_->set_level(toSpd(config.level));
  impl_->flush_on(spdlog::level::warn);
  level_ = config.level;
  return core::Status::success();
}

void Logger::setLevel(Level level) {
  level_ = level;
  if (impl_) impl_->set_level(toSpd(level));
}

void Logger::log(Level level, std::string_view module, std::string_view message,
                 core::ErrorCode code) {
  if (!impl_) return;
  if (code != core::ErrorCode::Ok) {
    impl_->log(toSpd(level), "[{}] {} (code={} {})", module, message, core::toInt(code),
               core::describe(code));
  } else {
    impl_->log(toSpd(level), "[{}] {}", module, message);
  }
}

void Logger::logCommand(Level level, std::string_view module, std::string_view command_id,
                        std::string_view message, core::ErrorCode code) {
  if (!impl_) return;
  if (code != core::ErrorCode::Ok) {
    impl_->log(toSpd(level), "[{}] [cmd={}] {} (code={} {})", module, command_id, message,
               core::toInt(code), core::describe(code));
  } else {
    impl_->log(toSpd(level), "[{}] [cmd={}] {}", module, command_id, message);
  }
}

void Logger::flush() {
  if (impl_) impl_->flush();
}

void Logger::shutdown() {
  if (impl_) {
    impl_->flush();
    impl_.reset();
  }
}

}  // namespace nexus::logging
