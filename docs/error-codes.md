# Error Codes

Canonical registry in `src/core/ErrorCodes.h`. Ranges are reserved per module so a code alone
identifies its origin. `core::describe(code)` returns a human string.

| Range | Module |
|---|---|
| 1000–1099 | core |
| 1100–1199 | logging |
| 1200–1299 | config |
| 1300–1399 | system / state machine |
| 1400–1499 | identity / crypto |

| Code | Name | Meaning |
|---|---|---|
| 0 | Ok | success |
| 1000 | Unknown | unknown error |
| 1001 | InvalidArg | invalid argument |
| 1002 | NotFound | not found |
| 1003 | IoError | I/O error |
| 1004 | Timeout | timeout |
| 1005 | PermissionDenied | permission denied |
| 1006 | Corrupt | corrupt data |
| 1007 | NotImplemented | not implemented |
| 1008 | AlreadyExists | already exists |
| 1100 | LogInitFailed | logger init failed |
| 1200 | ConfigNotFound | config file not found |
| 1201 | ConfigParseError | config parse error |
| 1202 | ConfigValidationFailed | config validation failed |
| 1203 | ConfigWriteFailed | config write failed |
| 1204 | ConfigCorrupt | config corrupt |
| 1300 | IllegalStateTransition | illegal state transition |
| 1301 | ServiceStartFailed | service start failed |
| 1400 | CryptoError | crypto error |
| 1401 | IdentityMissing | device identity missing |
| 1402 | IdentityCorrupt | device identity corrupt |
| 1403 | KeyGenerationFailed | key generation failed |
| 1404 | SigningFailed | signing failed |
| 1405 | SecureStoreError | secure store error |

Add new codes in the reserved range for the owning module; keep values stable (they are a
diagnostic contract).
