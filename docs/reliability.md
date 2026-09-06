# Updates, Watchdog & Safe Mode (Phase 9)

The reliability layer: signed OTA updates with rollback, a watchdog that recovers stuck services,
and a safe mode of last resort.

## Signed OTA update (`updater::UpdateManager`)

Flow: `download → verify signature → check compatibility → backup → install → restart →
health check → confirm / rollback`.

- **UpdatePackage**: payload + manifest (version, min_hardware_version, payload SHA-256, size). The
  manifest is signed by the vendor's Ed25519 key; the signature covers the manifest's canonical
  bytes, which include the payload hash — so a tampered payload fails verification.
- **SignatureValidator**: checks the payload SHA-256 matches the manifest AND the manifest
  signature verifies against the vendor public key (via `identity::crypto`). No unsigned or
  tampered package is installed.
- **Compatibility**: refuses a package whose `min_hardware_version` exceeds the device's.
- **RollbackManager**: backs up the current binary to `<target>.prev` before install; restores it
  if the post-install health check fails.
- **Installer**: atomic replace (temp write + fsync + rename, preserving 0755).
- Updates are refused while a calibration is running (`setUpdatesAllowed(false)` on
  CalibrationStarted). Emits UpdateStarted/Completed/Failed and RollbackTriggered.

The download source is behind `IUpdateSource` (StubUpdateSource for tests). The post-install
"restart + probe" is an injected `HealthCheckFn` (real impl: systemd restart + status probe).

## Watchdog (`system::Watchdog`)

A periodic loop calls `healthCheck()` on every registered service. A service that fails for
`fail_threshold` consecutive polls (default 3) triggers a recovery action and emits
`WatchdogTimeout`; each failing poll emits `HealthCheckFailed`. Recovering resets the counter. On
the Pi the loop also feeds the systemd hardware watchdog (`sd_notify WATCHDOG=1`).

## Safe Mode (`system::SafeMode`)

Entered on repeated watchdog timeouts, corrupt config, identity corruption, or repeated failed
updates. SystemManager drives it: the amplifier is muted and audio is stopped (injected disable
action), the system goes Degraded, and `SafeModeEntered` is emitted — but the web interface, logs,
and network stay up so the device can be recovered remotely (rollback / reset). Idempotent.
