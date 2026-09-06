# Nexus Speaker OS

Dedicated firmware for a physical audio speaker device (Raspberry Pi 4, Raspberry Pi OS
Bookworm). The speaker receives audio and **cryptographically signed commands** from a
*Streamer* over the local network, processes audio through a DSP chain, drives the amplifier
and microphone hardware, performs acoustic calibration, and reports status securely.

It does **not** connect to any SaaS/cloud service directly — the Streamer is the sole source
of commands.

## Architecture

One module = one concern. A fault in any single module must not crash the process. Modules
are decoupled through an in-process event bus and a shared `IService` lifecycle contract, so
`main` can bring every service up and down in a fixed order and the watchdog can monitor them
independently.

```
main → system → config → identity → network → discovery → pairing → control
     → audio → dsp → amplifier → microphone → calibration → status → diagnostics
     → updater → storage → logging → web
```

See [docs/architecture.md](docs/architecture.md) for the full module map, the
snake_case↔PascalCase naming table, and the event catalog.

## Building (development host)

Dependencies: CMake ≥ 3.20, a C++17 compiler, `libsodium`, `spdlog`, `nlohmann-json`.
GoogleTest is fetched automatically. On macOS: `brew install cmake libsodium spdlog nlohmann-json`.

```sh
cmake --preset host-debug
cmake --build build
ctest --test-dir build --output-on-failure
```

On a development machine the hardware layer is stubbed (`NEXUS_STUB_HAL=ON`) so the whole
project builds and unit-tests without real audio hardware.

## Status

All 10 build phases complete — every module is implemented (not stubbed):

1. Core scaffold — Logger, Config, State Machine, Device Identity, EventBus, IService
2. Network, Discovery, crypto Pairing (Ed25519/X25519 sealed-box Wi-Fi transfer)
3. Signed Command Server (TCP 45455) + Status/heartbeat
4. Synchronized audio receive, jitter buffer, playback (UDP 50005)
5. Real-time DSP chain (32-band EQ, crossover, compressor, limiter, delay, gain)
6. Amplifier (thermal/fault), Microphone (SPL/noise, measurement-only), Diagnostics
7. Automatic acoustic calibration (auto EQ + auto volume)
8. Local web interface + REST API (http/8080)
9. Signed OTA updates with rollback, Watchdog, Safe Mode
10. §31 mandatory scenario tests, soak/stability, gated hardware suite

**159 tests pass** on the host (`cmake --preset host-debug && ctest`). OS-specific work
(nmcli, Avahi, ALSA/I2S, GPIO, cpp-httplib) lives behind HALs; build for the device with the
`rpi-release` preset (`NEXUS_STUB_HAL=OFF`). See [docs/](docs/) for per-subsystem detail.
