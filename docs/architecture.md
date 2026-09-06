# Architecture

Nexus Speaker OS is a single native process composed of independent modules. Each module owns
one concern and implements the `nexus::core::IService` lifecycle contract. Modules never call each
other directly for state changes — they communicate through the in-process `EventBus`. This is
what makes fault isolation real: a subscriber that throws or hangs cannot take down a publisher,
and a non-critical service that fails to start is degraded rather than fatal.

## Layers

```
main (Application)          owns EventBus + all services; ordered startup/shutdown
  └── core                  IService, EventBus, Result/Status, ErrorCodes, SecretString, StubService
        ├── logging          spdlog facade + secret redaction   (Phase 1, full)
        ├── config           schema, validator, atomic manager  (Phase 1, full)
        ├── storage          SecureStorage (0600) + repo stubs   (Phase 1, SecureStorage full)
        ├── identity         Ed25519 keypair, provisioning, sign (Phase 1, full)
        ├── system           StateMachine + SystemManager        (Phase 1, full) + Watchdog/Health stubs
        └── (stub modules)   network, discovery, pairing, control, audio, dsp, amplifier,
                             microphone, calibration, status, diagnostics, updater, web
```

Only `core` is a shared "public" surface. Every other module depends on `core` (and, where it
logs, on `logging`) but not on its peers — cross-module interaction is via events. The CMake link
graph enforces this: a layering violation fails to link.

## Naming

Spec module/class names are snake_case; code uses PascalCase files/classes, mapped 1:1.

| Spec name | Class |
|---|---|
| `command_server` | `control::CommandServer` |
| `device_identity` | `identity::DeviceIdentity` |
| `state_machine` | `system::StateMachine` |
| `config_manager` | `config::ConfigManager` |
| `secure_storage` | `storage::SecureStorage` |
| `equalizer` | `dsp::Equalizer` |
| … | … (every spec class has a header under `src/<module>/`) |

## Startup order

`Application` starts services in the specification's order and stops them in reverse:

```
Logger → Config → Identity → System → Storage → Amplifier → Microphone → Network →
Discovery → Pairing → Control → DSP → Audio → Calibration → Status → Diagnostics →
Updater → Web → Watchdog
```

Critical services (Config, Identity) failing to start abort the boot with a controlled shutdown.
Any other service failing is logged and marked `Degraded`; the process continues.

## Event catalog

Events are defined in `src/core/Event.h`. Groups: system/lifecycle, identity/pairing,
network/discovery, control/audio, hardware/alerts, update/calibration. `SystemManager` subscribes
to the bus and maps inbound events to state-machine transitions (see [state-machine.md](state-machine.md)).

## Hardware abstraction

`amplifier`, `microphone`, and `audio` each define an `I*Hal` interface with a stub implementation
(dev hosts, `NEXUS_STUB_HAL=ON`) and, in later phases, an ALSA/GPIO-backed real implementation on
the Pi. This lets the entire system build and unit-test off-target.
