#include "calibration/CalibrationManager.h"

#include "logging/Logger.h"
#include "microphone/SplMeter.h"

using nexus::microphone::SplMeter;

namespace nexus::calibration {

using core::ErrorCode;
using core::Event;
using core::EventType;
using core::Result;
using core::ServiceState;
using core::Status;

const char* toString(CalState s) {
  switch (s) {
    case CalState::Idle: return "IDLE";
    case CalState::Preparing: return "PREPARING";
    case CalState::Measuring: return "MEASURING";
    case CalState::Analyzing: return "ANALYZING";
    case CalState::Applying: return "APPLYING";
    case CalState::Verifying: return "VERIFYING";
    case CalState::Completed: return "COMPLETED";
    case CalState::Failed: return "FAILED";
  }
  return "UNKNOWN";
}

CalibrationManager::CalibrationManager(core::EventBus* bus, config::ConfigManager* config,
                                       std::shared_ptr<CalibrationProfiles> profiles,
                                       double sample_rate)
    : bus_(bus), config_(config), profiles_(std::move(profiles)), room_(sample_rate) {}

Status CalibrationManager::start() {
  svc_state_ = ServiceState::Running;
  return Status::success();
}

Status CalibrationManager::stop() {
  svc_state_ = ServiceState::Stopped;
  return Status::success();
}

void CalibrationManager::setHooks(CaptureFn capture, ApplyEqFn apply_eq, MuteFn mute) {
  capture_ = std::move(capture);
  apply_eq_ = std::move(apply_eq);
  mute_ = std::move(mute);
}

CalState CalibrationManager::calState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cal_state_;
}

void CalibrationManager::setState(CalState s) {
  std::lock_guard<std::mutex> lock(mutex_);
  cal_state_ = s;
}

Result<CalibrationResult> CalibrationManager::runCalibration(const std::string& profile_name) {
  if (!capture_) return Status::error(ErrorCode::NotImplemented, "no capture hook");
  if (bus_) bus_->publish(Event{EventType::CalibrationStarted, "calibration"});
  NX_LOG_INFO("calibration", "starting calibration: " + profile_name);

  auto fail = [&](Status s) -> Result<CalibrationResult> {
    setState(CalState::Failed);
    NX_LOG_ERROR("calibration", s.code(), "calibration failed: " + s.message());
    if (bus_) bus_->publish(Event{EventType::CalibrationFailed, "calibration"});
    if (mute_) mute_(false);  // restore playback
    return s;
  };

  // 1-2. Mute normal playback, play the calibration signal + capture the room response.
  setState(CalState::Preparing);
  if (mute_) mute_(true);

  setState(CalState::Measuring);
  const std::size_t frames = static_cast<std::size_t>(room_.sampleRate());  // ~1s
  std::vector<std::int16_t> captured = capture_(frames);
  if (captured.empty()) return fail(Status::error(ErrorCode::IoError, "empty capture"));

  // 3-4. Analyze the frequency response into per-band energy.
  setState(CalState::Analyzing);
  auto samples = RoomMeasurement::toDouble(captured);
  auto band_db = room_.bandEnergyDb(samples);
  auto correction = auto_eq_.computeCorrection(band_db);
  const double score = AutoEq::flatnessScore(band_db);

  CalibrationResult result;
  result.recommended_eq = correction;
  result.score = score;
  result.room_noise_dbfs = SplMeter::rmsDbfs(captured);  // overall captured level

  // 5-6. Apply the correction to the DSP and verify it was accepted.
  setState(CalState::Applying);
  if (apply_eq_) apply_eq_(correction);

  setState(CalState::Verifying);
  // A basic validity check: gains within range (AutoEq already clamps).
  for (double g : correction) {
    if (g < -10.001 || g > 10.001) return fail(Status::error(ErrorCode::Corrupt, "gain out of range"));
  }

  // 7-8. Persist the profile and record it in config.
  CalibrationProfile profile;
  profile.name = profile_name;
  profile.eq_gains = correction;
  profile.score = score;
  if (profiles_) {
    Status s = profiles_->save(profile);
    if (!s.ok()) return fail(s);
  }
  if (config_) {
    config_->update([&](config::SpeakerConfig& c) { c.audio.eq_profile = profile_name; });
  }

  // 9. Restore playback.
  if (mute_) mute_(false);
  setState(CalState::Completed);
  NX_LOG_INFO("calibration",
              "calibration complete: score=" + std::to_string(static_cast<int>(score)));
  if (bus_) {
    bus_->publish(Event{EventType::CalibrationCompleted, "calibration",
                        {{"score", score}, {"profile", profile_name}}});
  }
  return result;
}

Status CalibrationManager::applySavedProfile(const std::string& profile_name) {
  if (!profiles_) return Status::error(ErrorCode::NotFound, "no profile store");
  auto p = profiles_->load(profile_name);
  if (!p.ok()) return p.status();
  if (apply_eq_) apply_eq_(p.value().eq_gains);
  NX_LOG_INFO("calibration", "applied saved profile: " + profile_name);
  return Status::success();
}

}  // namespace nexus::calibration
