# Installation (Raspberry Pi 4, Bookworm)

1. Install build + runtime deps:
   ```sh
   sudo apt install build-essential cmake pkg-config \
     libsodium-dev libspdlog-dev nlohmann-json3-dev \
     libasound2-dev libgpiod-dev libavahi-client-dev \
     network-manager
   ```
2. Build on-device with the real hardware backends:
   ```sh
   cmake -B build-rpi -DNEXUS_STUB_HAL=OFF -DNEXUS_BUILD_TESTS=OFF
   cmake --build build-rpi
   ```
   (Or cross-compile — see [build.md](build.md).)
3. `sudo ./scripts/install.sh`.
4. Enable the I2S audio overlay in `/boot/firmware/config.txt` for your DAC/amp HAT, e.g.:
   ```
   dtparam=i2s=on
   dtoverlay=hifiberry-dac
   ```
5. `systemctl start nexus-speaker`.

## Hardware backends (NEXUS_STUB_HAL=OFF)

- **Audio out** — `AlsaAudioOutputHal` (libasound), I2S PCM device from config (`hw:X,Y`).
- **Microphone** — `AlsaMicrophoneHal` (libasound capture); measurement only.
- **Amplifier** — `GpioAmplifierHal` (libgpiod): enable/mute output lines, fault/protection input
  lines (active-low), temperature from a sysfs thermal zone. Pin/zone assignments are set in the
  `GpioAmplifierHal::Pins` struct per board.
- **Discovery** — `AvahiDiscoveryHal`: publishes `_nexus-speaker._tcp` (TXT record with device_id,
  model, version, X25519 box key, control port, setup_mode) and browses `_nexus-streamer._tcp`.

The service runs as the non-root `nexus-speaker` user; network control uses a polkit rule, audio
via the `audio` group, GPIO/I2C via udev rules. See [operations.md](operations.md).
