#pragma once

namespace nexus::system {

// The 14 operating states from the specification.
enum class SystemState {
  Booting,
  Unconfigured,
  SetupMode,
  ConnectingNetwork,
  SearchingStreamer,
  Authenticating,
  Online,
  Playing,
  Calibrating,
  Updating,
  Degraded,
  Offline,
  Error,
  ShuttingDown,
};

const char* toString(SystemState s);

}  // namespace nexus::system
