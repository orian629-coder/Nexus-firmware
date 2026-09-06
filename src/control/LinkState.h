#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace nexus::control {

// Last link-strength report pushed by the paired streamer (via the REPORT_LINK command). This is
// ephemeral telemetry — it is NOT persisted to config; it only feeds the live status the kiosk
// polls. The CommandExecutor writes it when a REPORT_LINK arrives; the web ApiContext reads it to
// expose the streamer's signal (with a freshness flag so a stale report can fall back to the
// speaker's own RTT link meter). Thread-safe: the control TCP thread writes, the web thread reads.
//
// `epoch_now` is caller-supplied (injected clock) so freshness is testable and the class stays free
// of a hard dependency on wall-clock time.
class LinkState {
 public:
  struct Snapshot {
    bool valid = false;          // a report has been received at least once
    int wifi_signal_dbm = 0;     // negative dBm (e.g. -55); meaningful only when valid
    std::string streamer_id;     // who reported it
    std::int64_t reported_at = 0;  // epoch seconds of the last report
  };

  void report(int wifi_signal_dbm, const std::string& streamer_id, std::int64_t epoch_now) {
    std::lock_guard<std::mutex> lk(mutex_);
    snap_.valid = true;
    snap_.wifi_signal_dbm = wifi_signal_dbm;
    snap_.streamer_id = streamer_id;
    snap_.reported_at = epoch_now;
  }

  Snapshot get() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return snap_;
  }

 private:
  mutable std::mutex mutex_;
  Snapshot snap_;
};

}  // namespace nexus::control
