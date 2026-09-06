# System State Machine

14 states, enforced by a static transition table in `src/system/StateMachine.cpp`. Illegal
transitions are rejected (returned as `IllegalStateTransition` and logged) but never crash the
process. Every accepted transition emits a `StateChanged` event.

## States

`BOOTING, UNCONFIGURED, SETUP_MODE, CONNECTING_NETWORK, SEARCHING_STREAMER, AUTHENTICATING,
ONLINE, PLAYING, CALIBRATING, UPDATING, DEGRADED, OFFLINE, ERROR, SHUTTING_DOWN`

## Legal transitions

Any state may transition to `SHUTTING_DOWN` (always) and to `ERROR` (fault path, except from
`SHUTTING_DOWN`). Self-transitions are no-ops that succeed.

| From | Legal targets (beyond ERROR / SHUTTING_DOWN) |
|---|---|
| BOOTING | UNCONFIGURED, CONNECTING_NETWORK |
| UNCONFIGURED | SETUP_MODE |
| SETUP_MODE | CONNECTING_NETWORK, UNCONFIGURED |
| CONNECTING_NETWORK | SEARCHING_STREAMER, OFFLINE, SETUP_MODE |
| SEARCHING_STREAMER | AUTHENTICATING, OFFLINE, CONNECTING_NETWORK |
| AUTHENTICATING | ONLINE, SEARCHING_STREAMER, OFFLINE |
| ONLINE | PLAYING, CALIBRATING, UPDATING, DEGRADED, OFFLINE |
| PLAYING | ONLINE, CALIBRATING, DEGRADED, OFFLINE |
| CALIBRATING | ONLINE, PLAYING, DEGRADED |
| UPDATING | ONLINE |
| DEGRADED | ONLINE, OFFLINE |
| OFFLINE | CONNECTING_NETWORK, SEARCHING_STREAMER, DEGRADED |
| ERROR | BOOTING (recovery), DEGRADED |
| SHUTTING_DOWN | (terminal) |

## Event → transition mapping

`SystemManager` drives transitions from bus events (`src/system/SystemManager.cpp`):

| Event | Transition |
|---|---|
| NetworkConnected | → SEARCHING_STREAMER |
| NetworkDisconnected | → OFFLINE |
| StreamerFound | → AUTHENTICATING |
| PairingCompleted | → ONLINE |
| StreamerDisconnected | → OFFLINE |
| AudioStarted | → PLAYING |
| AudioStopped | → ONLINE |
| ConfigInvalid | → DEGRADED |
| IdentityCorrupt | → ERROR |
| ShutdownRequested | → SHUTTING_DOWN |

Recovery escalation (restart module → restart app → reboot → safe mode) attaches via
`StateMachine::onEnter` hooks in Phase 9.
