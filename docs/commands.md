# Commands

The speaker runs a **command server** on **TCP 45455** (`control` module). Every command is a JSON
object sent by the paired Streamer and **signed** with the Streamer's Ed25519 key. The speaker
verifies the signature against the streamer public key stored in `config.json` (set during
pairing) before executing.

See [protocol.md](protocol.md) for the transport and wire format.

## Pipeline

```
Receive → Parse → Idempotency check (command_id) → Validate + Authenticate (signature)
        → Execute → Return result → Store history
```

- **Idempotency**: a `command_id` seen before returns its cached result without re-executing
  (`CommandHistory`, LRU-bounded).
- **Authentication**: `CommandValidator` checks required fields, `target_id` match, `expires_at`,
  and the Ed25519 signature over `Command::canonicalString()`.
- **Result**: every command returns `{type:"command_result", command_id, ok, message, error_code,
  data}`.

## Wire format

```json
{
  "type": "command",
  "command_id": "cmd-12345",
  "target_id": "SPK-A104",
  "command": "SET_VOLUME",
  "payload": { "volume": 70 },
  "expires_at": 1785100300,
  "timestamp": 1785100000,
  "signature": "<base64 Ed25519 over canonicalString()>"
}
```

`canonicalString()` = `command_id\ntarget_id\ncommand\n<compact-payload>\nexpires_at\ntimestamp`
(no signature). Both sides must serialize the payload identically.

## Command set

| Command | Phase 3 behavior |
|---|---|
| GET_STATUS | returns volume/muted/delay/eq_profile/paired |
| SET_VOLUME | `{volume:0-100}` → persisted to config |
| SET_MUTE | `{muted:bool}` → persisted |
| SET_DELAY | `{delay_ms:int}` → persisted |
| SET_EQ | `{eq_profile:string}` → persisted (bands applied by DSP in Phase 5) |
| SET_GAIN, START/STOP/PAUSE/RESUME_AUDIO, RUN_CALIBRATION, RUN_SELF_TEST, RUN_AUDIO_TEST, UPDATE_SOFTWARE | accepted + emitted on the bus; executed by their modules as those phases land |
| REBOOT, RESET_NETWORK, FACTORY_RESET | accepted + emitted; REBOOT also raises ShutdownRequested |

Unknown commands, unsigned/tampered/expired commands, and wrong-target commands are rejected with
an error result.

## Status & heartbeat

The `status` module sends a **heartbeat** (default every 30 s) with device_id, state, volume,
muted, software_version, and timestamp. Critical events (amplifier overheat/fault/protection,
audio lost, streamer disconnected, update failed, mic failure, temp warning) are reported
**immediately** as alerts rather than waiting for the next heartbeat. Delivery is via a pluggable
sink so the streamer-push transport can be injected without changing the module.
