#pragma once

#include <cmath>
#include <string>

namespace nexus::measure {

// Speed of sound in dry air at 20 °C. Temperature-dependent: roughly +0.6 m/s per °C, so a room at
// 30 °C is ~2 % faster and a 5 m measurement would read ~10 cm short. Exposed so a caller with a
// thermometer can do better than the default.
constexpr double kSpeedOfSoundMps = 343.0;

inline double speedOfSoundAt(double celsius) { return 331.3 + 0.606 * celsius; }

// The fixed delays that are NOT flight time, and must come off the measured lag before it means
// anything about distance.
//
// Both terms are essential, and getting this wrong is the single most likely way to produce a
// confidently wrong number:
//
//   • hardware_rtl_ms — converter, driver and buffer latency of the loopback path. Measured once
//     per machine with an impulse (see Chirp.h) and reused.
//   • network_rtl_ms — the streaming path delay when the chirp is played THROUGH the system rather
//     than out of the local card. On this project that is ~43-50 ms of measured stream latency;
//     leaving it in would add ~15 metres to every reading.
//
// The AOA prototype subtracts both (`total_rtl = 210.7 + network_rtl`). Keeping them as separate
// named fields rather than one lumped constant means a change in streaming latency does not
// silently corrupt distances that were calibrated against a different buffer size.
struct LatencyBudget {
  double hardware_rtl_ms = 0.0;
  double network_rtl_ms = 0.0;

  double totalMs() const { return hardware_rtl_ms + network_rtl_ms; }
};

struct DistanceResult {
  double distance_m = 0.0;
  double acoustic_ms = 0.0;  // flight time after the fixed delays are removed
  bool valid = false;
  std::string note;  // why it is not valid, when it is not
};

// Convert a measured round-trip lag into a distance.
//
// A small negative acoustic time is expected and normal: it means the microphone is essentially AT
// the speaker and the RTL estimate is off by a fraction of a millisecond. That is reported as zero
// distance rather than as an error. A LARGE negative time is different — it means the latency
// budget is wrong (usually a stale RTL, or network_rtl left at zero), and silently clamping it to 0
// would hide a systematic error behind a plausible-looking number.
inline DistanceResult toDistance(double lag_ms, const LatencyBudget& budget,
                                 double speed_of_sound = kSpeedOfSoundMps) {
  DistanceResult r;
  r.acoustic_ms = lag_ms - budget.totalMs();

  // 1 ms ≈ 34 cm. Half a millisecond of slop is within the resolution this method can honestly
  // claim; beyond that the calibration itself is suspect.
  constexpr double kToleranceMs = 0.5;

  if (r.acoustic_ms < -kToleranceMs) {
    r.note = "measured lag is shorter than the configured latency budget — RTL is stale or "
             "network_rtl is unset";
    return r;
  }
  if (r.acoustic_ms < 0.0) r.acoustic_ms = 0.0;  // mic at the speaker, within tolerance

  r.distance_m = r.acoustic_ms * speed_of_sound / 1000.0;
  r.valid = true;
  return r;
}

}  // namespace nexus::measure
