#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/EventBus.h"
#include "core/IService.h"
#include "dsp/Compressor.h"
#include "dsp/Crossover.h"
#include "dsp/Delay.h"
#include "dsp/Equalizer.h"
#include "dsp/GainControl.h"
#include "dsp/Limiter.h"

namespace nexus::dsp {

// The real-time DSP chain applied to each audio block before output, in the spec's order:
//
//   Input Gain → High-Pass (crossover) → Equalizer (32-band) → Crossover (low-pass) →
//   Compressor → Limiter → Delay → Output Gain
//
// Processes interleaved 16-bit PCM: converts to float, runs the chain, converts back with
// clipping. Thread-safe configuration (the playback thread calls processInt16 while control
// commands may reconfigure). The engine is an IService so Application manages its lifecycle; the
// PlaybackManager calls processInt16() between the jitter buffer and the output HAL.
class DspEngine : public core::IService {
 public:
  explicit DspEngine(core::EventBus* bus, double sample_rate = 48000.0, int channels = 2);

  std::string name() const override { return "dsp"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  void configure(double sample_rate, int channels);

  // ── configuration (thread-safe) ──
  void setEqGains(const std::array<double, kEqBands>& gains_db);
  void setInputGainDb(double db);
  void setOutputGainDb(double db);
  void setDelayMs(double ms);
  void setLimiter(bool enabled, double threshold_db);
  void setCompressor(bool enabled, double threshold_db, double ratio);
  void setCrossover(bool enabled, bool hp, double hp_fc, bool lp, double lp_fc);
  void setBypass(bool on) { bypass_ = on; }

  // Flip the polarity of every sample. Used when a driver is wired backwards, or positioned so its
  // output arrives out of phase with its neighbours and cancels their bass. Atomic and applied per
  // block, so it can be toggled during playback while the installer listens for the null.
  void setPhaseInvert(bool on) { phase_invert_ = on; }
  bool phaseInvert() const { return phase_invert_.load(); }

  // Process a block of interleaved int16 PCM in place. `frames` = samples per channel.
  void processInt16(std::int16_t* io, std::size_t frames, int channels);

  void reset();

 private:
  core::EventBus* bus_;
  double fs_;
  int channels_;
  std::atomic<bool> bypass_{false};
  std::atomic<bool> phase_invert_{false};

  std::mutex mutex_;  // guards stage reconfiguration vs processing
  GainControl input_gain_;
  Crossover crossover_;
  Equalizer eq_;
  Compressor compressor_;
  Limiter limiter_;
  Delay delay_;
  GainControl output_gain_;

  std::vector<float> scratch_;  // reused float working buffer
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::dsp
