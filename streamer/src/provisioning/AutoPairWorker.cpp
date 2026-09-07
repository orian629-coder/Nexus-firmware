#include "provisioning/AutoPairWorker.h"
namespace nexus::streamer::provisioning {

std::string deriveSetupCode(const std::string& device_id) {
  if (device_id.size() <= 4 || device_id.compare(0, 4, "SPK-") != 0) return "";
  return "SETUP-" + device_id.substr(4);
}

AutoPairWorker::AutoPairWorker(discovery::IStreamerDiscovery& disc, ProvisioningWindow& win,
                               IsRegisteredFn is_registered, PairFn pair, std::function<std::int64_t()> now)
    : disc_(disc), win_(win), is_registered_(std::move(is_registered)),
      pair_(std::move(pair)), now_(std::move(now)) {}

int AutoPairWorker::sweepOnce() {
  const auto now = now_();
  if (!win_.isOpen(now)) return 0;
  auto result = disc_.browseSpeakers();
  if (!result) return 0;                        // browse failure → nothing this sweep
  int paired = 0;
  for (const auto& s : result.value()) {
    if (!s.setup_mode) continue;
    if (is_registered_(s.device_id)) continue;
    const std::string code = deriveSetupCode(s.device_id);
    if (code.empty()) continue;                 // malformed device_id → skip
    if (!win_.isOpen(now_())) break;            // window may have closed mid-sweep
    PairAttempt a{s.device_id, s.host, s.box_public_key, code};
    if (pair_(a)) ++paired;
  }
  return paired;
}
}  // namespace nexus::streamer::provisioning
