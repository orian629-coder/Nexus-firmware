#include "dsp/DspEngine.h"

#include <algorithm>

#include "logging/Logger.h"

namespace nexus::dsp {

using core::ServiceState;
using core::Status;

DspEngine::DspEngine(core::EventBus* bus, double sample_rate, int channels)
    : bus_(bus),
      fs_(sample_rate),
      channels_(channels),
      crossover_(sample_rate, channels),
      eq_(sample_rate, channels),
      compressor_(sample_rate),
      limiter_(sample_rate),
      delay_(sample_rate, channels) {}

Status DspEngine::start() {
  state_ = ServiceState::Running;
  return Status::success();
}

Status DspEngine::stop() {
  state_ = ServiceState::Stopped;
  return Status::success();
}

void DspEngine::configure(double sample_rate, int channels) {
  std::lock_guard<std::mutex> lock(mutex_);
  fs_ = sample_rate;
  channels_ = channels;
  crossover_.configure(sample_rate, channels);
  eq_.setSampleRate(sample_rate);
  eq_.setChannels(channels);
  compressor_.setSampleRate(sample_rate);
  limiter_.setSampleRate(sample_rate);
  delay_.configure(sample_rate, channels);
}

void DspEngine::setEqGains(const std::array<double, kEqBands>& gains_db) {
  std::lock_guard<std::mutex> lock(mutex_);
  eq_.setGains(gains_db);
}
void DspEngine::setInputGainDb(double db) {
  std::lock_guard<std::mutex> lock(mutex_);
  input_gain_.setGainDb(db);
}
void DspEngine::setOutputGainDb(double db) {
  std::lock_guard<std::mutex> lock(mutex_);
  output_gain_.setGainDb(db);
}
void DspEngine::setDelayMs(double ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  delay_.setDelayMs(ms);
}
void DspEngine::setLimiter(bool enabled, double threshold_db) {
  std::lock_guard<std::mutex> lock(mutex_);
  limiter_.setEnabled(enabled);
  limiter_.setThresholdDb(threshold_db);
}
void DspEngine::setCompressor(bool enabled, double threshold_db, double ratio) {
  std::lock_guard<std::mutex> lock(mutex_);
  compressor_.setEnabled(enabled);
  compressor_.setThresholdDb(threshold_db);
  compressor_.setRatio(ratio);
}
void DspEngine::setCrossover(bool enabled, bool hp, double hp_fc, bool lp, double lp_fc) {
  std::lock_guard<std::mutex> lock(mutex_);
  crossover_.setEnabled(enabled);
  crossover_.setHighPass(hp, hp_fc);
  crossover_.setLowPass(lp, lp_fc);
}

void DspEngine::processInt16(std::int16_t* io, std::size_t frames, int channels) {
  // Polarity is applied even under bypass, and before anything else.
  //
  // Bypass means "no tone shaping" — it is what an installer switches on to hear the source
  // untouched. Phase inversion is not tone shaping: it corrects a driver that is wired backwards or
  // positioned so it cancels its neighbours' bass. Dropping it under bypass would silently
  // reintroduce the cancellation the installer just corrected, and it would look like the bypass
  // switch itself broke the sound.
  if (phase_invert_.load()) {
    const std::size_t total = frames * static_cast<std::size_t>(channels);
    for (std::size_t i = 0; i < total; ++i) {
      // -32768 has no positive counterpart in int16; negating it overflows back to itself, so it is
      // clamped to +32767 instead of wrapping to a full-scale sample of the WRONG sign — which
      // would be an audible click on exactly the loudest samples.
      io[i] = (io[i] == -32768) ? 32767 : static_cast<std::int16_t>(-io[i]);
    }
  }

  if (bypass_.load()) return;
  std::lock_guard<std::mutex> lock(mutex_);

  const std::size_t n = frames * static_cast<std::size_t>(channels);
  scratch_.resize(n);
  // int16 → float [-1, 1]
  for (std::size_t i = 0; i < n; ++i) scratch_[i] = static_cast<float>(io[i]) / 32768.0f;

  float* buf = scratch_.data();
  input_gain_.process(buf, frames, channels);   // Input Gain
  crossover_.process(buf, frames, channels);     // High-Pass + Low-Pass
  eq_.process(buf, frames);                       // 32-band EQ
  compressor_.process(buf, frames, channels);     // Compressor
  limiter_.process(buf, frames, channels);        // Limiter (output protection)
  delay_.process(buf, frames);                    // Delay
  output_gain_.process(buf, frames, channels);    // Output Gain

  // float → int16 with clipping
  for (std::size_t i = 0; i < n; ++i) {
    float v = std::clamp(scratch_[i], -1.0f, 1.0f);
    io[i] = static_cast<std::int16_t>(v * 32767.0f);
  }
}

void DspEngine::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  input_gain_.reset();
  crossover_.reset();
  eq_.reset();
  compressor_.reset();
  limiter_.reset();
  delay_.reset();
  output_gain_.reset();
}

}  // namespace nexus::dsp
