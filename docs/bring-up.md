# Raspberry Pi Bring-Up Checklist

From a bare Raspberry Pi 4 to a working Nexus Speaker. Do these in order.

## 1. Base OS

- Flash **Raspberry Pi OS Lite (64-bit, Bookworm)** with Raspberry Pi Imager.
- In the imager's advanced options: set a hostname (e.g. `speaker-a104`), enable SSH, set the
  Wi-Fi/locale (Wi-Fi is optional — pairing transfers credentials later, but SSH access needs
  network for the first build).
- Boot, then `ssh <user>@<hostname>.local`.

## 2. Hardware wiring

- Mount the I2S DAC/amp HAT (e.g. HiFiBerry, MAX98357A) and, if used, the I2S/MEMS mic.
- Wire the amplifier's **mute/enable** and **fault/protection** lines to GPIO (defaults in
  `GpioAmplifierHal::Pins`: enable=17, mute=27, fault=22, protect=23 — adjust for your board).

## 3. Enable the audio overlay

Append `deploy/rpi/config.txt.append` (pick the overlay matching your HAT) to
`/boot/firmware/config.txt`, then reboot. Verify the card appears:

```sh
aplay -l          # should list the I2S card
arecord -l        # should list the mic, if fitted
```

## 4. Build + install

From your dev machine (recommended — one command):

```sh
scripts/deploy.sh <user>@<hostname>.local
```

This rsyncs the source, installs deps, builds with the real HALs, and runs the installer. Or, on
the device directly:

```sh
sudo apt install build-essential cmake pkg-config libsodium-dev libspdlog-dev \
  nlohmann-json3-dev libasound2-dev libgpiod-dev libavahi-client-dev network-manager \
  bluez pipewire-audio wireplumber libspa-0.2-bluetooth
./scripts/build-rpi.sh          # native build → build-rpi/
sudo ./scripts/install.sh       # user, dirs, polkit, udev, systemd unit
```

Install is idempotent: it never overwrites an existing config or device identity.

The last four packages enable the Bluetooth A2DP sink: `bluez` provides `bluetoothctl`, and
`pipewire-audio`/`wireplumber`/`libspa-0.2-bluetooth` route a connected phone's audio to the I2S
output. The service user is added to the `bluetooth` group (see the sysusers/systemd unit) so it
can reach BlueZ over the D-Bus system bus. To verify after boot: `journalctl -u nexus-speaker | grep -i bluetooth`
should show `A2DP sink ready`, and the speaker appears as **Nexus Audio** in a phone's Bluetooth list.

## 5. First boot

```sh
sudo systemctl start nexus-speaker
journalctl -u nexus-speaker -f
```

Expected on first boot:

- `[identity] provisioning new device identity` → a `SPK-XXXXXXXX` id (once; preserved forever).
- `[amplifier] amp OFF -> STARTING` (powers on muted).
- `[web] web interface on http/8080`.
- State reaches `UNCONFIGURED` (no pairing yet).

## 6. Verify

- **Web UI**: `http://<hostname>.local:8080` shows device/audio/network/hardware.
- **Discovery**: `avahi-browse -rt _nexus-speaker._tcp` from another host shows the beacon.
- **Diagnostics**: `./scripts/diagnostics.sh` (redacted status/health/logs).
- **Audio path**: once paired to a Streamer and audio flows, the amp unmutes and PCM reaches the
  I2S sink; `Soak`-style continuous playback should stay glitch-free.

## 7. Pairing

The Streamer discovers the speaker via mDNS, the technician enters the setup code, and the Streamer
sends a signed pairing request with sealed Wi-Fi credentials. On success the speaker persists the
pairing (public info in config, Wi-Fi PSK in SecureStorage), joins the network, finds the Streamer,
and reaches `ONLINE`. See [crypto.md](crypto.md) and [protocol.md](protocol.md).

## 8. Field maintenance

- **Update**: signed OTA via the Streamer or `scripts/update.sh` (verifies signature, rolls back on
  a failed health check).
- **Factory reset**: `sudo ./scripts/reset.sh` — wipes config/pairing/state, **preserves the device
  identity + keypair** so it re-onboards without re-issuing keys.
- **Recovery**: on repeated crashes / corrupt config the device enters **Safe Mode** (amp muted,
  playback off) but keeps the web UI, logs, and network up for remote rollback/reset.
