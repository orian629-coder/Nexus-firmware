#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "dsp/DspEngine.h"
#include "dsp/Equalizer.h"

namespace nexus::streamer::dsp {

// The streamer's MASTER processing chain: the global stage of the two-tier design.
//
//   source → [MasterDsp: input gain, 32-band EQ, limiter, master volume] → packetize → speakers
//                                                                              ↓
//                                          each speaker then applies its OWN gain/delay/EQ
//
// This is deliberately the "global" half only. Per-speaker delay, gain and room correction stay on
// the speaker, where they belong: applying them here would mean sending a different stream per
// speaker, which defeats the single multicast-style fan-out the send pipeline is built around.
//
// Wraps the SPEAKER's nexus::dsp::DspEngine rather than reimplementing filters. That library is
// already tested and running on hardware, and sharing it means the streamer's EQ and a speaker's EQ
// cannot drift apart in behavior — the same reason the streamer lives in the speaker's repo.
struct MasterDspConfig {
  bool bypass = false;
  double input_gain_db = 0.0;
  double master_volume_db = 0.0;
  bool limiter_enabled = true;      // on by default: the master bus feeds N speakers, and a clipped
  double limiter_threshold_db = -1.0;  // master is clipped everywhere at once
  std::array<double, nexus::dsp::kEqBands> eq_gains_db{};  // value-initialized → all 0 dB (flat)

  nlohmann::json toJson() const {
    nlohmann::json eq = nlohmann::json::array();
    for (double g : eq_gains_db) eq.push_back(g);
    return {{"bypass", bypass},
            {"input_gain_db", input_gain_db},
            {"master_volume_db", master_volume_db},
            {"limiter_enabled", limiter_enabled},
            {"limiter_threshold_db", limiter_threshold_db},
            {"eq_gains_db", std::move(eq)}};
  }

  // Lenient by design: a config from an older schema, or a hand-edited file, must still yield a
  // usable chain rather than refusing to start. Unknown/missing fields keep their defaults.
  static MasterDspConfig fromJson(const nlohmann::json& j) {
    MasterDspConfig c;
    if (!j.is_object()) return c;
    c.bypass = j.value("bypass", c.bypass);
    c.input_gain_db = j.value("input_gain_db", c.input_gain_db);
    c.master_volume_db = j.value("master_volume_db", c.master_volume_db);
    c.limiter_enabled = j.value("limiter_enabled", c.limiter_enabled);
    c.limiter_threshold_db = j.value("limiter_threshold_db", c.limiter_threshold_db);
    if (j.contains("eq_gains_db") && j["eq_gains_db"].is_array()) {
      const auto& arr = j["eq_gains_db"];
      // A wrong-length array fills what it can and leaves the rest flat, rather than throwing.
      for (std::size_t i = 0; i < arr.size() && i < c.eq_gains_db.size(); ++i) {
        if (arr[i].is_number()) c.eq_gains_db[i] = arr[i].get<double>();
      }
    }
    return c;
  }
};

// Clamp an EQ gain to the same ±10 dB range the speaker's Equalizer enforces, so the streamer
// cannot request a curve the speaker would silently refuse to reproduce.
inline double clampEqGainDb(double db) {
  if (db > 10.0) return 10.0;
  if (db < -10.0) return -10.0;
  return db;
}

class MasterDsp {
 public:
  explicit MasterDsp(double sample_rate = 48000.0, int channels = 2)
      : engine_(std::make_unique<nexus::dsp::DspEngine>(nullptr, sample_rate, channels)) {
    engine_->start();
    apply(config_);
  }

  ~MasterDsp() { engine_->stop(); }

  // Called on the AUDIO thread, once per block, in place.
  void process(std::int16_t* io, std::size_t frames, int channels) {
    engine_->processInt16(io, frames, channels);
  }

  // Called on the WEB thread. DspEngine guards its own stages, so no extra lock is needed around
  // the engine itself; the mutex here only keeps `config_` consistent for readers of config().
  void setConfig(const MasterDspConfig& incoming) {
    MasterDspConfig c = incoming;
    for (auto& g : c.eq_gains_db) g = clampEqGainDb(g);
    {
      std::lock_guard<std::mutex> lk(mutex_);
      config_ = c;
    }
    apply(c);
  }

  MasterDspConfig config() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return config_;
  }

 private:
  void apply(const MasterDspConfig& c) {
    engine_->setInputGainDb(c.input_gain_db);
    engine_->setOutputGainDb(c.master_volume_db);
    engine_->setLimiter(c.limiter_enabled, c.limiter_threshold_db);
    engine_->setEqGains(c.eq_gains_db);
    // Set last so that when bypass is being turned OFF, every stage is already configured before
    // audio starts flowing through the chain. Each setter takes DspEngine's own lock individually,
    // so a block in flight may still observe some stages updated and others not — audibly a brief
    // crossfade between two valid curves, never a click, since the gain stages ramp per-sample.
    engine_->setBypass(c.bypass);
  }

  std::unique_ptr<nexus::dsp::DspEngine> engine_;
  mutable std::mutex mutex_;
  MasterDspConfig config_;
};

}  // namespace nexus::streamer::dsp
