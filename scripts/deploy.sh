#!/usr/bin/env bash
# Deploy Nexus Speaker OS to a Raspberry Pi over SSH.
#
# The default (and most robust) path builds NATIVELY on the Pi: it rsyncs the source tree to the
# device, installs build deps, compiles with the real HALs, and runs the installer. The Pi 4
# compiles the project comfortably.
#
# Usage:
#   scripts/deploy.sh pi@speaker.local
#   scripts/deploy.sh pi@10.0.0.50 --no-deps      # skip apt (deps already installed)
set -euo pipefail

TARGET="${1:-}"
if [[ -z "$TARGET" ]]; then
  echo "Usage: deploy.sh <user@host> [--no-deps]" >&2
  exit 1
fi
INSTALL_DEPS=1
[[ "${2:-}" == "--no-deps" ]] && INSTALL_DEPS=0

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Resolve the remote home over SSH so this works whether TARGET is "user@host" or an ssh-config
# alias (e.g. "speaker", whose real user/home isn't derivable from the alias string).
REMOTE_HOME="$(ssh "$TARGET" 'echo "$HOME"')"
REMOTE_DIR="${REMOTE_HOME:-~}/nexus-speaker-src"

echo "==> Syncing source to $TARGET:$REMOTE_DIR"
rsync -az --delete \
  --exclude 'build/' --exclude 'build-rpi/' --exclude '.git/' \
  "$REPO_ROOT/" "$TARGET:$REMOTE_DIR/"

if [[ $INSTALL_DEPS -eq 1 ]]; then
  echo "==> Installing build + runtime dependencies on the device"
  # build deps (compile with real HALs) + runtime deps for the Bluetooth A2DP sink:
  # bluez provides bluetoothctl; pipewire/wireplumber + libspa-0.2-bluetooth route the incoming
  # A2DP stream to the I2S output.
  ssh "$TARGET" 'sudo apt-get update && sudo apt-get install -y \
    build-essential cmake pkg-config \
    libsodium-dev libspdlog-dev nlohmann-json3-dev \
    libasound2-dev libgpiod-dev libavahi-client-dev \
    network-manager nftables \
    bluez pipewire-audio wireplumber libspa-0.2-bluetooth \
    python3-dbus python3-gi'
fi
# nftables backs the captive-portal port-80→8080 redirect in scripts/hotspot.sh. Debian 13 (trixie)
# ships neither nft nor iptables by default, so without this the setup page never auto-pops — the
# phone's captive probe reaches nothing. (A userspace port-80 forwarder is the fallback, but the
# nft redirect is the fast path.)
# python3-dbus + python3-gi (GLib) back the BLE provisioning helper (provisioning/ble_provisioning.py).

# Wipe the previous build tree before rebuilding.
#
# Two reasons, both learned the hard way on this Pi. The card is 6.8 GB and a build tree is ~800 MB,
# so leaving the old one behind repeatedly pushed the disk to 100% — and a compile that dies with
# "No space left on device" leaves the OLD binary running while the deploy appears to have
# progressed. Wiping also guarantees the installed binary is built from the synced source and not
# from a stale object file.
#
# Only build artefacts are removed. The installed binary, /etc/nexus-speaker (config, pairing,
# identity) and the systemd unit are untouched, so the speaker keeps serving until the new binary
# is installed and stays paired afterwards.
echo "==> Removing the previous build tree"
ssh "$TARGET" "sudo rm -rf $REMOTE_DIR/build-rpi $REMOTE_DIR/build 2>/dev/null; sudo apt-get clean 2>/dev/null; df -h / | tail -1"

echo "==> Building on the device (real HALs)"
# NEXUS_BUILD_STREAMER=OFF: the streamer runs on the controller machine, never on a speaker, so
# building it here only consumes disk. That matters — a 8GB Pi card filled to 100% mid-build on
# 2026-08-24 and the compile died with "No space left on device", leaving the previous binary
# running while the deploy appeared to be progressing.
ssh "$TARGET" "cd $REMOTE_DIR && cmake -B build-rpi -DNEXUS_STUB_HAL=OFF -DNEXUS_BUILD_TESTS=OFF -DNEXUS_BUILD_STREAMER=OFF && cmake --build build-rpi -j\$(nproc)"

echo "==> Installing (systemd unit, user, dirs, polkit)"
# install.sh expects the built binary at build/src/main/nexus-speaker; point it at build-rpi.
ssh "$TARGET" "cd $REMOTE_DIR && sudo ln -sfn build-rpi build && sudo ./scripts/install.sh"

echo "==> Installing the HDMI kiosk display (display.py + Nexus logo)"
# The kiosk is a separate unit installed by its own script — deploying the server alone leaves an
# old display.py running. Run it here so one deploy updates both the server and the on-screen UI.
ssh "$TARGET" "cd $REMOTE_DIR && sudo ./scripts/install-kiosk.sh"

# Restart the service so the freshly installed binary is the one actually running. Without this the
# deploy silently leaves the OLD process up: install.sh replaces the file on disk but systemd keeps
# executing the already-loaded image, so a verification right after deploying tests the previous
# build and appears to show the fix not working. The kiosk installer already restarts its own unit.
# Remove the build tree now that the binary is installed.
#
# Wiping only BEFORE the build is not enough: the fresh tree is ~800 MB and leaves this 6.8 GB card
# at 99%, so the next deploy starts against a nearly full disk and a compile can die with "No space
# left on device" — which leaves the OLD binary running while the deploy looks like it progressed.
# Clearing it here means the speaker idles with ~800 MB free instead of ~100 MB.
#
# The cost is that every deploy is a full rebuild (~8 min) rather than incremental. On a card this
# size that is the right trade: a failed deploy has cost more time here than rebuilds do.
echo "==> Reclaiming build space"
ssh "$TARGET" "sudo rm -rf $REMOTE_DIR/build-rpi $REMOTE_DIR/build 2>/dev/null; df -h / | tail -1"

echo "==> Restarting nexus-speaker"
ssh "$TARGET" "sudo systemctl restart nexus-speaker && sleep 3 && systemctl is-active nexus-speaker"

echo "==> Deploy complete. Running build:"
ssh "$TARGET" "systemctl show nexus-speaker -p MainPID --value | xargs -I{} ps -o lstart= -p {} 2>/dev/null | sed 's/^/    started: /'"
echo "    Logs:   journalctl -u nexus-speaker -f"
echo "    Web UI: http://<device>:8080"
