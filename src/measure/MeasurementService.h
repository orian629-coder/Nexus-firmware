#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "measure/ArrivalDetector.h"
#include "measure/Chirp.h"
#include "measure/Distance.h"

namespace nexus::measure {

// One microphone's answer for one speaker.
struct MicMeasurement {
  int mic_index = 0;          // 0-based; "Mic1" in the UI is index 0
  double distance_m = 0.0;
  double lag_ms = 0.0;        // raw round-trip before the latency budget is removed
  double confidence = 0.0;    // peak / next-best-peak
  bool valid = false;
  std::string note;
};

struct MeasurementResult {
  std::string device_id;
  std::vector<MicMeasurement> mics;
  bool ok = false;
  std::string message;
};

// What to measure and with what corrections.
struct MeasurementRequest {
  double duration_s = 1.0;
  double sample_rate = 48000.0;
  double f0 = 1000.0;
  double f1 = 15000.0;
  double amplitude = 0.4;   // the prototype used 0.05 over the network and 0.4 locally
  LatencyBudget budget;     // hardware + network RTL, both required for a correct distance
  double speed_of_sound = kSpeedOfSoundMps;
  // Which mics to use. Empty means every channel the capture returns.
  std::vector<int> mic_indices;
};

// Runs an acoustic measurement ON THE SPEAKER: play a chirp out of this speaker's own output while
// recording the microphones attached to it, then correlate to get per-mic arrival times.
//
// The play-and-record has to happen on the same device for the numbers to mean anything: the lag is
// measured against the moment the sound left THIS speaker, and routing the stimulus over the
// network first would fold streaming jitter into every reading. This mirrors the AOA prototype,
// where each Pi ran its own listener and returned only the finished distances.
//
// Hardware access is injected as a single hook, matching CalibrationManager's design, so the
// orchestration is testable with synthetic audio and the module stays free of any ALSA dependency.
class MeasurementService {
 public:
  // Play `stimulus` through this speaker's output while capturing its microphones. Returns
  // INTERLEAVED PCM with `channels` channels — the same shape the prototype got from
  // `playrec(channels=2, input_mapping=[1,2])`, which is what allows two mics to be measured from
  // one emission rather than two.
  //
  // Playing and recording must start together; a hook that records after playing would measure
  // nothing but the tail of the room.
  struct Capture {
    std::vector<std::int16_t> pcm;
    int channels = 1;
  };
  using PlayRecordFn = std::function<Capture(const std::vector<double>& stimulus)>;

  explicit MeasurementService(PlayRecordFn play_record)
      : play_record_(std::move(play_record)) {}

  MeasurementResult measure(const MeasurementRequest& req) const {
    MeasurementResult out;

    if (!play_record_) {
      out.message = "no capture backend on this device";
      return out;
    }
    if (req.sample_rate <= 0.0 || req.duration_s <= 0.0) {
      out.message = "invalid measurement parameters";
      return out;
    }

    const std::vector<double> stimulus =
        logChirp(req.duration_s, req.sample_rate, req.f0, req.f1, req.amplitude);
    if (stimulus.empty()) {
      out.message = "stimulus is empty";
      return out;
    }

    const Capture cap = play_record_(stimulus);
    if (cap.pcm.empty() || cap.channels <= 0) {
      out.message = "capture returned no audio";
      return out;
    }

    const std::size_t frames = cap.pcm.size() / static_cast<std::size_t>(cap.channels);
    if (frames < stimulus.size()) {
      out.message = "capture shorter than the stimulus";
      return out;
    }

    std::vector<int> mics = req.mic_indices;
    if (mics.empty()) {
      for (int c = 0; c < cap.channels; ++c) mics.push_back(c);
    }

    // The search is bounded by the budget plus the longest flight time worth considering. Beyond
    // that a "peak" is a room reflection or noise, not the direct arrival, and searching for it
    // only costs time and invites a wrong answer.
    constexpr double kMaxRoomMetres = 30.0;
    const double max_ms = req.budget.totalMs() + (kMaxRoomMetres / req.speed_of_sound) * 1000.0;
    const long max_lag = static_cast<long>(max_ms * req.sample_rate / 1000.0);

    for (int mic : mics) {
      MicMeasurement m;
      m.mic_index = mic;

      if (mic < 0 || mic >= cap.channels) {
        m.note = "no such microphone channel";
        out.mics.push_back(std::move(m));
        continue;
      }

      // De-interleave this microphone's channel.
      std::vector<double> ch(frames);
      for (std::size_t i = 0; i < frames; ++i) {
        ch[i] = static_cast<double>(cap.pcm[i * static_cast<std::size_t>(cap.channels) +
                                            static_cast<std::size_t>(mic)]) /
                32768.0;
      }

      const Arrival a = findArrival(ch, stimulus, req.sample_rate, max_lag);
      if (!a.found) {
        m.note = "no arrival detected (silence, or the mic is not hearing this speaker)";
        out.mics.push_back(std::move(m));
        continue;
      }

      m.lag_ms = a.lag_ms;
      m.confidence = a.confidence();

      const DistanceResult d = toDistance(a.lag_ms, req.budget, req.speed_of_sound);
      if (!d.valid) {
        m.note = d.note;
        out.mics.push_back(std::move(m));
        continue;
      }

      m.distance_m = d.distance_m;
      m.valid = true;
      out.mics.push_back(std::move(m));
    }

    // Partial success is still success: with several mics, one that cannot hear this speaker should
    // not discard the readings from the others. The caller decides whether it has enough.
    for (const auto& m : out.mics) {
      if (m.valid) {
        out.ok = true;
        break;
      }
    }
    if (!out.ok) out.message = "no microphone produced a usable measurement";
    return out;
  }

 private:
  PlayRecordFn play_record_;
};

}  // namespace nexus::measure
