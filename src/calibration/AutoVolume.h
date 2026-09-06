#pragma once

#include <algorithm>
#include <cmath>

namespace nexus::calibration {

// Adjusts playback volume to stay a fixed offset above ambient noise (spec: target = ambient +
// offset, default +2 dB). Protections: gradual change only (bounded step per update), a maximum
// volume clamp, and a hold time between changes (enforced by the caller's poll interval). Pure and
// testable. Volume is expressed on the 0..100 config scale; the mapping from ambient dBFS to a
// volume level uses a simple linear model tuned by `db_to_volume`.
class AutoVolume {
 public:
  struct Params {
    double offset_db = 2.0;       // target = ambient + offset
    int max_volume = 90;          // never exceed
    int min_volume = 10;          // never mute via auto
    int max_step = 3;             // max volume change per update (gradual)
    double db_to_volume = 1.0;    // volume units per dB of ambient level
    double ambient_ref_dbfs = -60.0;  // ambient level mapping to the min volume anchor
  };

  AutoVolume() : p_(Params{}) {}
  explicit AutoVolume(Params p) : p_(p) {}

  // Given the current volume and the current ambient level (dBFS), return the next volume,
  // stepping gradually toward the target and respecting the min/max clamps.
  int nextVolume(int current_volume, double ambient_dbfs) const {
    // Map ambient level to a target volume: louder room → higher target.
    const double target_raw =
        static_cast<double>(p_.min_volume) +
        (ambient_dbfs - p_.ambient_ref_dbfs + p_.offset_db) * p_.db_to_volume;
    int target = static_cast<int>(std::lround(target_raw));
    target = std::clamp(target, p_.min_volume, p_.max_volume);

    // Gradual step toward target.
    int delta = std::clamp(target - current_volume, -p_.max_step, p_.max_step);
    int next = current_volume + delta;
    return std::clamp(next, p_.min_volume, p_.max_volume);
  }

  const Params& params() const { return p_; }

 private:
  Params p_;
};

}  // namespace nexus::calibration
