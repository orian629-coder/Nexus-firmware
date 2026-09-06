# DSP Chain (Phase 5)

Real-time audio processing applied to each block between the jitter buffer and the output, in the
spec's order:

```
Input Gain → High-Pass → Equalizer (32-band) → Crossover (Low-Pass) → Compressor → Limiter
           → Delay → Output Gain
```

`DspEngine::processInt16` converts interleaved 16-bit PCM to float, runs the chain, and converts
back with clipping. Configuration is thread-safe (the playback thread processes while control
commands reconfigure).

## Stages

- **Biquad** (`Biquad.h`) — RBJ cookbook peaking EQ + second-order Butterworth HP/LP, Direct Form
  II Transposed. Ported from the prior speaker-app. `magnitudeDb()` supports verification.
- **Equalizer** — 32 ISO center frequencies (30 Hz – 20 kHz), per-band gain clamped to ±10 dB,
  independent biquad state per channel.
- **GainControl** — input/output gain in dB, per-sample smoothed to avoid zipper noise.
- **Crossover** — optional Butterworth HP (protect the woofer) + LP (driver band-limit).
- **Compressor** — threshold/ratio/attack/release/makeup, linked peak detection.
- **Limiter** — peak limiter with attack/release; caps the signal to protect the amp/driver.
- **Delay** — per-channel ring-buffer delay (ms) for multi-speaker time alignment.

## Integration

The DSP engine is wired into playback via `PlaybackManager::setDspHook` — a
`std::function<void(int16_t*, frames, channels)>` — so the `audio` module stays decoupled from the
`dsp` module. The volume/mute stage runs after the DSP chain. Bypass is available for A/B testing.

Config maps to stages: `audio.eq_profile` → EQ band gains (Phase 7 calibration writes these),
`audio.delay_ms` → Delay, and the limiter/compressor/crossover from the `dsp` config section.
