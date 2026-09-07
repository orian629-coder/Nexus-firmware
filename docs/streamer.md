# Nexus Streamer

The Streamer is the audio sender + controller counterpart to the speaker (a pure playback sink). It
lives in [streamer/](../streamer/) in this repo and links the speaker's contract code directly
(packet encoder, signed-command struct, crypto), so the wire format matches by construction.

Architecture note (vs. the AOA reference): AOA routed audio **locally** (BlackHole → local speaker)
and only sent text commands over the network. The Nexus streamer instead **sends timestamped audio
over the network** to N speakers that release each packet on their NTP-synced clock — giving true
sample-aligned multi-room. Commands are signed JSON (Ed25519), not plain text.

## Build

```
cmake --preset host-debug        # builds the streamer with real sockets (NEXUS_STREAMER_REAL_NET=ON)
cmake --build build --target nexus-streamer
```

The streamer's networking is plain BSD sockets, so it builds and runs on a dev Mac as well as the
Pi — the speaker HAL stays stubbed on the host.

## Modes

```
nexus-streamer --serve [port]        # browser control UI (default 8090)
nexus-streamer --stream <ip[,ip2,…]> # stream raw PCM from stdin to speaker(s)
nexus-streamer --tone <ip>           # send a 440 Hz test tone (wire smoke test)
```

## Real capture → real playback (end to end)

The streamer reads **raw s16le / 48 kHz / stereo PCM from stdin** ([StdinPcmSource](../streamer/src/sources/StdinPcmSource.h)),
packetizes it with the speaker's own `packPacket`, timestamps each 10 ms block in the future, and
fans it out over UDP 50005 to every target. Anything that emits raw PCM can drive it:

macOS system audio (via [BlackHole](https://github.com/ExistentialAudio/BlackHole), like AOA):
```
ffmpeg -f avfoundation -i ":BlackHole 2ch" -ar 48000 -ac 2 -f s16le - \
  | nexus-streamer --stream 10.0.0.5
```

A file:
```
ffmpeg -i song.flac -ar 48000 -ac 2 -f s16le - | nexus-streamer --stream 10.0.0.5
```

Multi-room (same timestamped datagrams to several speakers → sample-aligned playback):
```
… -f s16le - | nexus-streamer --stream 10.0.0.5,10.0.0.6,10.0.0.7
```

Pi capture (PipeWire):
```
pw-record --rate 48000 --channels 2 --format s16 - | nexus-streamer --stream <ip>
```

### Prerequisites for audible playback
1. A speaker (real Pi build, `NEXUS_STUB_HAL=OFF`) running and reachable at `<ip>`.
2. **Shared clock**: the streamer host and every speaker synced to the same NTP source (chrony).
   The streamer stamps ~180 ms in the future; without a shared clock the speaker drops or delays
   audio. On an isolated LAN, run a local chrony server on the streamer and point speakers at it.
3. For commands (volume/mute/EQ/transport) to be accepted, the speaker must be **paired** to the
   streamer's Ed25519 key (see pairing below).

## Browser control UI

`nexus-streamer --serve` starts an HTTP server (default 8090) with a self-contained Hebrew/RTL
control UI at `/`. It manages a list of speakers and turns UI actions into **signed** commands via
`CommandClient` over TCP 45455. API: `GET/POST /api/speakers`, `POST /api/{status,volume,mute,eq,
delay,transport}` (each takes `{"speaker":"<device_id>", …}`).

## Components

| Concern | Files |
|---|---|
| Live capture | [sources/StdinPcmSource.h](../streamer/src/sources/StdinPcmSource.h) |
| Packetize + timestamp + UDP fan-out | [send/](../streamer/src/send/) (`Packetizer`, `Timestamper`, `UdpPacketSink`) |
| Signed commands | [control/](../streamer/src/control/) (`CommandSigner`, `CommandClient`, `TcpLineTransport`) |
| Pairing | [pairing/PairingClient](../streamer/src/pairing/) (seal Wi-Fi + sign request) |
| Discovery | [discovery/](../streamer/src/discovery/) (advertise `_nexus-streamer._tcp`, browse speakers) |
| Speaker registry + web control | [group/SpeakerRegistry.h](../streamer/src/group/SpeakerRegistry.h), [web/](../streamer/src/web/) |

## Identity & pairing

The streamer has a **persistent Ed25519 identity** ([StreamerIdentity](../streamer/src/identity/StreamerIdentity.h)):
a 0600 key file (`$NEXUS_STREAMER_KEY`, else `~/.nexus-streamer/identity.key`) generated once and
reused across runs, so a paired speaker keeps accepting this streamer's signed commands. `--serve`
prints the streamer id (`STR-xxxxxxxx`) and public key on startup.

Pair a speaker from the UI (the "צימוד רמקול חדש" card) or the API — the speaker must be in setup
mode showing a code:
```
GET  /api/discover   → speakers in setup mode on the LAN [{device_id,host,box_public_key,setup_mode}]
POST /api/pair       → {host, setup_code, box_public_key, wifi_ssid, wifi_psk[, name, site_id]}
```
The UI's "🔍 סרוק רמקולים" button calls `/api/discover` (mDNS) and auto-fills host + box_public_key;
you then enter the setup code and Wi-Fi and pair. Discovery uses Avahi and is available on the Pi;
on a dev host without Avahi `/api/discover` returns 501 and you enter host/box_public_key manually.
On success the speaker is added to the registry and immediately controllable. The streamer's
identity fields (id, public/secret key) are injected server-side; the UI never sees the secret.

## Shared clock (NTP) for multi-room

The wire protocol carries an absolute playback timestamp, so every device must agree on the wall
clock. [scripts/setup-ntp.sh](../scripts/setup-ntp.sh) configures chrony:

```
sudo scripts/setup-ntp.sh server                # streamer host: serve time to the LAN
sudo scripts/setup-ntp.sh client <streamer-ip>  # each speaker: sync to the streamer
```

On a normal LAN with internet, plain public NTP on all devices is already enough. On an isolated
LAN, run the `server` role on the streamer host (or one Pi) and point the speakers at it — the
server serves its own clock (`local stratum 10`) so they converge without internet. When the
streamer is a Mac, either keep the Mac + speakers on the same public NTP, or run `server` on a Pi
and sync the others (and the Mac via `sntp`) to it. Verify with `chronyc tracking` / `chronyc sources`.

## Deploy on the Pi

Build + install the streamer natively on a Pi (real HAL not required — the streamer is sockets +
capture only):
```
cmake -B build-rpi -DNEXUS_STUB_HAL=OFF -DNEXUS_BUILD_TESTS=OFF && cmake --build build-rpi
sudo install -m0755 build-rpi/streamer/nexus-streamer /usr/local/bin/
sudo install -m0644 deploy/nexus-streamer.service /etc/systemd/system/
sudo systemctl enable --now nexus-streamer     # control UI on :8090

# Private-AP (Phase A: speakers join Nexus-<streamer_id>). Requires wired eth0 uplink for internet.
sudo install -m0755 scripts/streamer-ap.sh /usr/local/bin/nexus-streamer-ap.sh
sudo install -m0644 deploy/nexus-streamer-ap.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nexus-streamer-ap.service     # brings up Nexus-<streamer_id> on wlan0
```

*Bringing up the AP takes `wlan0`; the streamer's internet must come from `eth0` (wired). The no-cable AP+STA 'juggle' fallback is Phase B (spike-gated) — not installed here.*

The audio-send pipeline is launched on demand from a capture source, e.g.
`pw-record --rate 48000 --channels 2 --format s16 - | nexus-streamer --stream <ips>`.

## Status / gaps
- Multi-room fan-out sends identical datagrams; per-room delay tuning is via `SET_DELAY`.
- Verified end to end on loopback (bytes match through the speaker's decoder; commands + pairing
  accepted by the real speaker services). Audible Pi playback needs hardware + shared NTP as above.
- Speaker discovery for the pairing card (auto-filling host/box_public_key from mDNS) is wired on
  the Pi via Avahi; the UI currently takes them manually.
