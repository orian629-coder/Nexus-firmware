# Calibration (Phase 7)

Automatic acoustic calibration measures the in-room frequency response and computes an EQ
correction that flattens it — the product's core feature, similar to Sonos Trueplay.

## Flow (`calibration::CalibrationManager`)

State machine: `IDLE → PREPARING → MEASURING → ANALYZING → APPLYING → VERIFYING → COMPLETED /
FAILED`.

```
mute playback → play calibration signal → capture mic → analyze (FFT → band energy → inverse EQ)
             → apply to DSP → verify → save profile → restore playback
```

The hardware-touching steps are injected as hooks so the orchestration is fully testable and the
module stays decoupled from audio/dsp/mic:

- **capture** — `MicrophoneManager::capture()` (the mic is measurement-only).
- **apply EQ** — `DspEngine::setEqGains()`.
- **mute** — `AmplifierManager::mute()/unmute()` around the measurement.

## Analysis

- **Fft** (`Fft.h`) — self-contained radix-2 Cooley-Tukey FFT (no FFTW dependency; the prior
  speaker-app used FFTW). Hann-windowed magnitude spectrum.
- **RoomMeasurement** — averages the spectrum into per-band energy (dB) at the 32 EQ center
  frequencies.
- **AutoEq** — inverts the measured deviation from the average level into a correction curve
  (boost quiet bands, cut loud bands), clamped to ±10 dB. `flatnessScore()` rates the result.
- **AutoVolume** — target volume = ambient noise (from `NoiseMonitor`) + offset (default +2 dB),
  changed gradually with a max-volume clamp.

## Persistence

`CalibrationProfiles` saves the correction gains + score as JSON under
`/var/lib/nexus-speaker/calibration`, atomically. On boot, Application re-applies the saved profile
named by `config.audio.eq_profile` so the room correction survives reboot. `RUN_CALIBRATION`
(Phase 3 command) triggers `runCalibration()`.
