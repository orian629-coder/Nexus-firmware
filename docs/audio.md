# Audio Pipeline (Phase 4)

The speaker receives a synchronized audio stream from the Streamer over UDP and plays it out with
multi-room sample alignment.

## Wire format (`audio::AudioPacket`)

UDP datagrams, each a 24-byte big-endian header + interleaved 16-bit PCM:

| Field | Type | Meaning |
|---|---|---|
| timestamp | double (8B) | absolute target playback time (epoch seconds) |
| sequence | uint64 (8B) | monotonic sequence number (loss/order detection) |
| frame_count | uint32 (4B) | PCM frames in the payload |
| flags | uint32 (4B) | bit0 = last packet of the stream |

Payload: `frame_count * channels` int16 samples (little-endian). Default format 48 kHz / 16-bit /
stereo. Adapted from the prior speaker-app SyncProtocol; NTP/clock sync is an OS concern (chrony).

## Modules

- **AudioReceiver** (IService) — receives datagrams via `IAudioSource` (StubAudioSource for tests,
  UdpAudioSource on the Pi, port 50005), decodes them, feeds the jitter buffer, updates sync, and
  emits `AudioStarted` / `AudioStopped`.
- **AudioBuffer** — jitter buffer ordered by arrival: prefill gate before playback, packet-loss
  detection from sequence gaps, overflow (drop oldest) and underflow counting. Exposes
  `BufferMetrics` for telemetry.
- **StreamSync** — smooths the offset between packet timestamps and the local clock (EWMA), and
  decides when a packet is `due` for playback (honoring a target buffer). Relies on OS NTP for the
  baseline.
- **PlaybackManager** — pops due packets, applies volume/mute from config as linear gain, and
  writes PCM to `IAudioOutputHal` (StubAudioOutputHal for tests, ALSA/I2S on the Pi). The DSP chain
  (Phase 5) inserts between the buffer and the output.

## Flow

```
UDP → IAudioSource → AudioReceiver → AudioBuffer → PlaybackManager → IAudioOutputHal
                          │              │              │
                          └── StreamSync ┘         volume/mute (config)
```

Everything except the socket and the ALSA sink is pure and unit-tested off-target.
