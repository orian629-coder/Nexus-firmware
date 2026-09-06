# Local Web Interface & API (Phase 8)

The speaker serves a technician dashboard and a REST API on **http/8080** (`web` module). The
transport is behind `IWebTransport` (StubWebTransport for tests, cpp-httplib on the Pi), so all
routing/auth logic is unit-tested without a socket.

- **Dashboard**: `GET /` — a self-contained HTML page (no external assets) that polls the API and
  renders device/audio/network/hardware status. Works offline on the speaker.
- **Auth**: read endpoints are open on the trusted LAN; state-changing `POST` endpoints require an
  `Authorization: Bearer <token>` header (constant-time compared). The token is provisioned per
  device.

## Endpoints

Read (open):

| Method | Path | Returns |
|---|---|---|
| GET | /api/status | device_id, state, version, volume, muted, eq_profile, paired |
| GET | /api/health | diagnostics report (aggregated checks) |
| GET | /api/network | connected, ip |
| GET | /api/audio | volume, muted, delay_ms, eq_profile |
| GET | /api/hardware | amplifier state, calibration state, streaming |
| GET | /api/calibration/status | calibration state |
| GET | /api/calibration/result | active eq_profile |
| GET | /api/system/logs | recent log lines (text/plain) |

Write (require Bearer token):

| Method | Path | Body |
|---|---|---|
| POST | /api/audio/volume | `{volume:0-100}` |
| POST | /api/audio/mute | `{muted:bool}` |
| POST | /api/audio/eq | `{eq_profile:string}` |
| POST | /api/audio/delay | `{delay_ms:int}` |
| POST | /api/audio/test | — |
| POST | /api/calibration/start | — (runs calibration) |
| POST | /api/system/reboot | — (raises ShutdownRequested) |
| POST | /api/system/update | — |
| POST | /api/system/reset-network | — |
| POST | /api/system/factory-reset | — (preserves identity/keys) |

Responses are JSON: reads return the resource; writes return `{ok, message}`. Unknown paths →
404, bad JSON → 400, missing/invalid token on a write → 401.

## Design

`ApiRouter` is a pure `(method, path, body) → response` function using an injected `ApiContext` of
callbacks, so the `web` module has no direct dependency on any other module. Application builds the
context from config/identity/network/amplifier/calibration/diagnostics and reuses the same
config-update logic as the signed command path.
