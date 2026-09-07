#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "discovery/IStreamerDiscovery.h"
#include "provisioning/ProvisioningWindow.h"
namespace nexus::streamer::provisioning {

std::string deriveSetupCode(const std::string& device_id);  // isolated trust anchor

struct PairAttempt { std::string device_id, host, box_public_key, setup_code; };
using PairFn = std::function<bool(const PairAttempt&)>;
using IsRegisteredFn = std::function<bool(const std::string&)>;

class AutoPairWorker {
 public:
  AutoPairWorker(discovery::IStreamerDiscovery& disc, ProvisioningWindow& win,
                 IsRegisteredFn is_registered, PairFn pair, std::function<std::int64_t()> now);
  int sweepOnce();
 private:
  discovery::IStreamerDiscovery& disc_;
  ProvisioningWindow& win_;
  IsRegisteredFn is_registered_;
  PairFn pair_;
  std::function<std::int64_t()> now_;
};
}  // namespace nexus::streamer::provisioning
