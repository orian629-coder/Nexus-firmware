#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace nexus::streamer::audio {

// A signal level meter, written from the AUDIO thread and read from the WEB thread.
//
// Two values per channel, because they answer different questions:
//   • RMS  — perceived loudness; what a VU bar should show.
//   • peak — the true sample maximum; the only value that reveals clipping, which RMS hides.
//
// Lock-free by construction: the audio thread only ever stores, the web thread only ever loads,
// and every field is an independent atomic. A reader can observe RMS from one block and peak from
// the next; for a meter refreshed many times a second that is invisible, and it is the reason the
// audio thread never waits on a UI request.
//
// Levels are kept as linear 0..1 floats; conversion to dBFS happens at the edge (JSON), so the
// hot path does no logarithms.
class LevelMeter {
 public:
  // Measure one block of interleaved int16 PCM. Cost is one pass, no allocation.
  void measure(const std::int16_t* pcm, std::size_t frames, int channels) {
    if (!pcm || frames == 0 || channels <= 0) return;

    double sum_sq_l = 0.0, sum_sq_r = 0.0;
    std::int32_t peak_l = 0, peak_r = 0;

    for (std::size_t f = 0; f < frames; ++f) {
      const std::int32_t l = pcm[f * static_cast<std::size_t>(channels)];
      // Mono sources are metered as a single channel mirrored into both bars, rather than leaving
      // the right bar dead.
      const std::int32_t r =
          channels > 1 ? pcm[f * static_cast<std::size_t>(channels) + 1] : l;
      sum_sq_l += static_cast<double>(l) * l;
      sum_sq_r += static_cast<double>(r) * r;
      peak_l = std::max(peak_l, std::abs(l));
      peak_r = std::max(peak_r, std::abs(r));
    }

    constexpr double kFullScale = 32768.0;
    const double n = static_cast<double>(frames);
    rms_l_.store(static_cast<float>(std::sqrt(sum_sq_l / n) / kFullScale),
                 std::memory_order_relaxed);
    rms_r_.store(static_cast<float>(std::sqrt(sum_sq_r / n) / kFullScale),
                 std::memory_order_relaxed);
    peak_l_.store(static_cast<float>(peak_l / kFullScale), std::memory_order_relaxed);
    peak_r_.store(static_cast<float>(peak_r / kFullScale), std::memory_order_relaxed);
    active_.store(true, std::memory_order_relaxed);
  }

  // Called when the stream stops, so the bars fall to silence instead of freezing at the last
  // measured level — a frozen meter reads as "signal present" and is worse than no meter.
  void reset() {
    rms_l_.store(0.0f, std::memory_order_relaxed);
    rms_r_.store(0.0f, std::memory_order_relaxed);
    peak_l_.store(0.0f, std::memory_order_relaxed);
    peak_r_.store(0.0f, std::memory_order_relaxed);
    active_.store(false, std::memory_order_relaxed);
  }

  struct Levels {
    float rms_left = 0.0f;    // linear 0..1
    float rms_right = 0.0f;
    float peak_left = 0.0f;
    float peak_right = 0.0f;
    bool active = false;      // false once the stream stops
  };

  Levels read() const {
    Levels v;
    v.rms_left = rms_l_.load(std::memory_order_relaxed);
    v.rms_right = rms_r_.load(std::memory_order_relaxed);
    v.peak_left = peak_l_.load(std::memory_order_relaxed);
    v.peak_right = peak_r_.load(std::memory_order_relaxed);
    v.active = active_.load(std::memory_order_relaxed);
    return v;
  }

 private:
  std::atomic<float> rms_l_{0.0f};
  std::atomic<float> rms_r_{0.0f};
  std::atomic<float> peak_l_{0.0f};
  std::atomic<float> peak_r_{0.0f};
  std::atomic<bool> active_{false};
};

// Linear amplitude (0..1) → dBFS, floored at `floor_db`. Digital silence is exactly 0 and has no
// logarithm, so it must be special-cased rather than allowed to produce -inf, which serializes to
// invalid JSON (null) and breaks the meter in the browser.
inline double toDbfs(float linear, double floor_db = -60.0) {
  if (linear <= 0.0f) return floor_db;
  const double db = 20.0 * std::log10(static_cast<double>(linear));
  return db < floor_db ? floor_db : db;
}

// dBFS → a 0..1 bar fraction for the UI, where `floor_db` is the bottom of the scale. Kept next to
// toDbfs so the meter's scale is defined in one place rather than duplicated in JavaScript.
inline double dbToFraction(double dbfs, double floor_db = -60.0) {
  if (dbfs <= floor_db) return 0.0;
  if (dbfs >= 0.0) return 1.0;
  return (dbfs - floor_db) / (0.0 - floor_db);
}

}  // namespace nexus::streamer::audio
