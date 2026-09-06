# Testing (Phase 10)

The suite runs on the host with the stub HALs (`cmake --preset host-debug && ctest`), plus a gated
hardware suite for the Pi.

## Layers

- **Unit tests** — one per module (`tests/unit/<module>_test.cpp`), wired via `nexus_add_test()`.
  Cover the pure logic of every module.
- **Integration tests** (`tests/integration/`) — cross-module flows over the real EventBus for each
  phase: onboarding→ONLINE, signed command + heartbeat, audio pipeline, DSP shaping, health
  monitoring + diagnostics, calibration apply+persist, web API, watchdog→safe-mode, OTA rollback.
- **Scenario tests** (`tests/end_to_end/scenario_test.cpp`) — the §31 mandatory scenarios.
- **Soak tests** (`tests/end_to_end/soak_test.cpp`) — time-scaled 24h playback + 7-day stability.
- **Script tests** — `reset.sh` preserves identity (runs under a sandbox prefix, no root).
- **Hardware tests** (`tests/hardware/`) — gated by `NEXUS_HW_TESTS=ON` + `NEXUS_ON_TARGET=1`; SKIP
  off-target.

## §31 mandatory tests — coverage

| Spec test | Where |
|---|---|
| Boot | Scenario.Boot + EndToEnd.FullBootAndCleanShutdown |
| Network Disconnect | Scenario.NetworkDisconnect |
| Streamer Disconnect | Scenario.StreamerDisconnect |
| Audio Packet Loss | Scenario.AudioPacketLoss |
| Power Failure | Scenario.PowerFailureRecovery (atomic writes survive unclean shutdown) |
| Update Failure | UpdateManager.RollsBackOnHealthCheckFailure + Integration.Phase9FailedUpdateRollsBack |
| DSP Failure | Scenario.DspFailureDegradesGracefully |
| Amplifier Overheat | Scenario.AmplifierOverheat + AmplifierManager.OverheatMutesAndAlerts |
| Microphone Failure | MicrophoneManager.SelfTestFailsAndAlertsOnCaptureError |
| Factory Reset | Scenario.FactoryResetPreservesIdentity + script_reset_preserves_identity |
| 24-Hour Playback | Soak.ContinuousPlaybackStaysStable (proxy; full run with NEXUS_SOAK=1) |
| 7-Day Stability | Soak.LongRunStateAndEventChurnStaysConsistent (proxy; full run with NEXUS_SOAK=1) |

## Running the long/hardware variants

```sh
# Full-length soak (hours) instead of the fast proxy:
NEXUS_SOAK=1 ctest --test-dir build -R Soak

# On the Pi with real hardware:
cmake -B build-rpi -DNEXUS_STUB_HAL=OFF -DNEXUS_HW_TESTS=ON
cmake --build build-rpi
NEXUS_ON_TARGET=1 ctest --test-dir build-rpi -R Hardware
```
