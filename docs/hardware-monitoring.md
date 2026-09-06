# Amplifier, Microphone & Diagnostics (Phase 6)

## Amplifier (`amplifier::AmplifierManager`)

State machine: `OFF → STARTING → READY / MUTED → PROTECTION / OVERHEATED / FAULT`.

- **Boot behavior**: powers on **muted** (STARTING), unmutes only once stable (`unmute()` → READY).
  On shutdown it mutes then powers off.
- **Thermal**: `poll()` reads temperature. ≥ warning threshold (default 70 °C) → `TempWarning`
  (once); ≥ shutdown threshold (85 °C) → mute + OVERHEATED + `AmplifierOverheat`. Cooling back below
  warning recovers to MUTED.
- **Faults**: hardware fault/protection flags → mute + FAULT/PROTECTION + `AmplifierFault` /
  `AmplifierProtection`. Clipping → `OutputClipping`. Cannot unmute while faulted.
- All hardware access is behind `IAmplifierHal` (StubAmplifierHal off-target; ALSA/GPIO on the Pi).

## Microphone (`microphone::MicrophoneManager`)

Used **only** for measurement/calibration/monitoring — it never streams captured audio out.

- `SplMeter` — RMS → dBFS, plus a reference offset that calibration (Phase 7) tunes to true SPL.
- `NoiseMonitor` — EWMA ambient-noise estimate, feeds Auto Volume (Phase 7).
- `selfTest()` — verifies the capture path yields samples; emits `MicFailure` on error.
- `capture()` — raw PCM for calibration; never leaves the device.
- Capture behind `IMicrophoneHal` (stub generates deterministic pseudo-noise off-target).

## Diagnostics (`diagnostics::DiagnosticService`)

Runs a set of registered named checks (amplifier, microphone, network, config — wired in
Application as probes so diagnostics never hard-depends on those modules) and aggregates them into
a structured report:

```json
{ "test_id": "diag-1001", "status": "completed", "result": "warning",
  "checks": { "network": "ok", "microphone": "warning", "amplifier": "ok" } }
```

Overall result = worst individual check. A throwing probe becomes an error. `RUN_SELF_TEST` /
`RUN_AUDIO_TEST` commands (Phase 3) will drive `run()` and return the report.

Critical hardware events (overheat, fault, protection, mic failure) flow through the EventBus to
`StatusService`, which forwards them to the Streamer as **immediate alerts** without waiting for
the next heartbeat.
