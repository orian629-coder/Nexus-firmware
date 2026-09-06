# Cryptography & Identity

All crypto uses **libsodium**. The design goal: the device has a permanent Ed25519 identity whose
private key never leaves the device, and (in later phases) commands are signed and Wi-Fi
credentials are transferred encrypted.

## Device identity (Phase 1, implemented)

- **Keypair**: Ed25519, generated on first boot via `crypto_sign_keypair` (`identity/KeyManager`).
- **Private key** (`device_ed25519_sk`, 64 bytes): stored only in `SecureStorage`
  (`/var/lib/nexus-speaker/secure/`, file `0600`, dir `0700`). Loaded into a `sodium_malloc`
  locked buffer for signing and wiped immediately after. There is **no** API that returns the
  private key, and no logging overload accepts key bytes.
- **Public key**: base64, stored in `factory.json` (public, safe to expose).
- **device_id**: `SPK-<hex>` derived from CSPRNG randomness (`randombytes_buf`) — **never** from
  IP or MAC. Persisted in `/etc/nexus-speaker/identity/factory.json` (`0444`, root-owned).
- **Signing**: `DeviceIdentity::sign()` produces a detached Ed25519 signature (base64). Verified in
  tests against libsodium directly.

## Factory reset preserves identity

`scripts/reset.sh` clears config, pairing, profiles, and state, but never
`/etc/nexus-speaker/identity/` or the secure keypair. The same physical speaker re-onboards
without re-issuing keys. Enforced by a unit test (`DeviceIdentity.FactoryResetPreservesIdentity`).

## Pairing (Phase 2, implemented)

- The device provisions a second keypair on first boot: an **X25519 encryption keypair**
  (`device_x25519_sk` in SecureStorage) alongside the Ed25519 signing key. Its public key is
  advertised in the setup beacon.
- **Setup mode** (`pairing::PairingService::beginSetupMode`) activates a time-limited setup code.
- The Streamer sends a `PairingRequest` carrying its Ed25519 public key, the setup code, sealed
  Wi-Fi credentials (`crypto_box_seal` to the speaker's X25519 key), and an Ed25519 **signature**
  over the canonical request bytes.
- `PairingValidator` checks the setup code + expiry and verifies the signature (proving the sender
  holds the advertised key). The speaker then opens the sealed box with its X25519 secret key to
  recover the Wi-Fi credentials.
- **Persistence**: only public pairing info (streamer id + public key + `paired` flag) goes to
  `config.json`; the Wi-Fi PSK/SSID go to SecureStorage. The PSK never appears in config or logs
  (verified by a unit test). Pairing is one-time — setup mode closes on success.
- `identity::crypto` provides the pure, tested primitives: `verifyEd25519`, `sealTo`/`sealOpen`,
  base64 helpers.

## Later phases

- **Signed commands (Phase 3)**: every command on TCP:45455 carries an Ed25519 signature over its
  canonical bytes, plus `command_id` and `expires_at`; the `control` module validates signature,
  freshness, and idempotency before executing.
- **Signed updates (Phase 9)**: OTA packages are signature-verified before install, with rollback.
