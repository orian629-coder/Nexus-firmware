#!/usr/bin/env bash
# Install Nexus Speaker OS on a Raspberry Pi (Bookworm). Idempotent: never overwrites an existing
# config or device identity.
set -euo pipefail

PREFIX=/usr/local/bin
ETC=/etc/nexus-speaker
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ $EUID -ne 0 ]]; then
  echo "install.sh must run as root" >&2
  exit 1
fi

echo "==> Creating service user"
install -m 0644 "$REPO_ROOT/deploy/nexus-speaker.sysusers.conf" /usr/lib/sysusers.d/nexus-speaker.conf
systemd-sysusers || useradd -r -s /usr/sbin/nologin -G audio,gpio,i2c,spi,bluetooth nexus-speaker || true

echo "==> Creating runtime directories"
install -m 0644 "$REPO_ROOT/deploy/nexus-speaker.tmpfiles.conf" /usr/lib/tmpfiles.d/nexus-speaker.conf
systemd-tmpfiles --create

echo "==> Installing binary"
# Prefer the device build (build-rpi), fall back to a generic build dir.
BIN=""
for cand in "$REPO_ROOT/build-rpi/src/main/nexus-speaker" "$REPO_ROOT/build/src/main/nexus-speaker"; do
  [[ -f "$cand" ]] && BIN="$cand" && break
done
if [[ -z "$BIN" ]]; then
  echo "error: no built binary found — run scripts/build-rpi.sh first" >&2
  exit 1
fi
install -m 0755 "$BIN" "$PREFIX/nexus-speaker"

echo "==> Installing default config (only if absent)"
mkdir -p "$ETC"
if [[ ! -f "$ETC/config.json" ]]; then
  install -m 0644 "$REPO_ROOT/config/production.json" "$ETC/config.json"
  chown nexus-speaker:nexus-speaker "$ETC/config.json"
else
  echo "    config.json exists — left untouched"
fi

echo "==> Installing udev rules"
install -m 0644 "$REPO_ROOT/deploy/udev/99-nexus-speaker.rules" /etc/udev/rules.d/99-nexus-speaker.rules
udevadm control --reload-rules && udevadm trigger || true

echo "==> Installing polkit rule for network control (nmcli without root)"
cat > /etc/polkit-1/rules.d/50-nexus-speaker.rules <<'EOF'
// Allow the nexus-speaker service user to manage network connections via NetworkManager.
polkit.addRule(function(action, subject) {
  if (subject.user == "nexus-speaker" &&
      action.id.indexOf("org.freedesktop.NetworkManager.") == 0) {
    return polkit.Result.YES;
  }
});
EOF

echo "==> Enabling the Bluetooth adapter for the A2DP sink"
# The Pi's onboard controller boots soft-blocked by rfkill and BlueZ does not auto-power it, so
# the speaker's enableSink() fails with "adapter state uncertain". Fix both, idempotently:
#  1) AutoEnable=true → BlueZ powers on any controller it finds (survives reboots by config).
#  2) clear the rfkill soft-block via sysfs (no rfkill binary needed) and let systemd-rfkill
#     persist the unblocked state across reboots.
BT_CONF=/etc/bluetooth/main.conf
if [[ -f "$BT_CONF" ]]; then
  if grep -qiE '^\s*#?\s*AutoEnable\s*=' "$BT_CONF"; then
    sed -i -E 's/^\s*#?\s*AutoEnable\s*=.*/AutoEnable=true/I' "$BT_CONF"
  else
    # No AutoEnable key present: append under [Policy] (or create the section).
    grep -qi '^\[Policy\]' "$BT_CONF" || printf '\n[Policy]\n' >> "$BT_CONF"
    printf 'AutoEnable=true\n' >> "$BT_CONF"
  fi
fi
for f in /sys/class/rfkill/rfkill*; do
  [[ -e "$f/type" ]] || continue
  if [[ "$(cat "$f/type")" == "bluetooth" ]]; then echo 0 > "$f/soft" 2>/dev/null || true; fi
done
systemctl enable systemd-rfkill.service 2>/dev/null || true
systemctl restart bluetooth 2>/dev/null || true

echo "==> Provisioning the PipeWire audio graph (headless A2DP → output routing)"
# Sets up PipeWire/WirePlumber as the speaker user's services with linger + a seat-less Bluetooth
# monitor, so a paired phone's A2DP stream reaches the speaker output. Idempotent.
"$REPO_ROOT/scripts/setup-audio.sh" nexus-speaker || \
  echo "    WARNING: audio provisioning failed — Bluetooth playback may not route (see setup-audio.sh)"

echo "==> Installing the BLE provisioning helper"
# BLE GATT onboarding channel (Just Works). Runs on demand — nexus-speaker's ProvisioningController
# starts/stops it based on connectivity — so the unit is installed but not wanted by any target.
if [[ -f "$REPO_ROOT/provisioning/ble_provisioning.py" ]]; then
  install -m 0755 "$REPO_ROOT/provisioning/ble_provisioning.py" /usr/local/bin/nexus-provisioning.py
  install -m 0644 "$REPO_ROOT/deploy/nexus-provisioning.service" \
    /etc/systemd/system/nexus-provisioning.service
  # Grant the speaker service user permission to start/stop the provisioning unit via systemctl.
  cat > /etc/polkit-1/rules.d/51-nexus-provisioning.rules <<'EOF'
// Allow the nexus-speaker service user to start/stop the BLE provisioning unit (ProvisioningController).
polkit.addRule(function(action, subject) {
  if (subject.user == "nexus-speaker" &&
      action.id == "org.freedesktop.systemd1.manage-units" &&
      action.lookup("unit") == "nexus-provisioning.service") {
    return polkit.Result.YES;
  }
});
EOF
else
  echo "    NOTE: provisioning/ble_provisioning.py not found — BLE onboarding not installed"
fi

echo "==> Installing the Wi-Fi setup hotspot (captive portal)"
# Wi-Fi AP for onboarding phones that can't use Web Bluetooth (iOS). Also on-demand, controlled by
# ProvisioningController alongside the BLE channel.
# The captive-portal redirect needs a packet-filter backend. Debian 13 (trixie) ships nftables and
# no longer installs iptables by default; hotspot.sh prefers nft, so make sure it's present.
if ! command -v nft >/dev/null 2>&1 && ! command -v iptables >/dev/null 2>&1; then
  echo "    installing nftables (needed for the captive-portal redirect)"
  apt-get install -y -qq nftables || echo "    NOTE: could not install nftables — captive-portal auto-open may not work"
fi
install -m 0755 "$REPO_ROOT/scripts/hotspot.sh" /usr/local/bin/nexus-hotspot.sh
# Userspace port-80 forwarder — the fallback path for the captive-portal redirect when the nft/
# iptables rule can't be installed. hotspot.sh looks for it at this exact path and warns if absent.
install -m 0755 "$REPO_ROOT/scripts/port80_forward.py" /usr/local/bin/nexus-port80-forward.py
install -m 0644 "$REPO_ROOT/deploy/nexus-hotspot.service" /etc/systemd/system/nexus-hotspot.service
cat > /etc/polkit-1/rules.d/52-nexus-hotspot.rules <<'EOF'
// Allow the nexus-speaker service user to start/stop the Wi-Fi setup hotspot (ProvisioningController).
polkit.addRule(function(action, subject) {
  if (subject.user == "nexus-speaker" &&
      action.id == "org.freedesktop.systemd1.manage-units" &&
      action.lookup("unit") == "nexus-hotspot.service") {
    return polkit.Result.YES;
  }
});
EOF

echo "==> Installing systemd unit"
install -m 0644 "$REPO_ROOT/deploy/nexus-speaker.service" /etc/systemd/system/nexus-speaker.service
systemctl daemon-reload
systemctl enable nexus-speaker.service

echo "==> Installing the streamer-AP boot join"
# Runs as root at boot, ordered Before=nexus-speaker.service, so a PAIRED speaker joins the streamer's
# private AP (Nexus-<streamer_id>, a clean subnet where mDNS works) before the app starts. Unpaired ->
# the script no-ops and normal onboarding proceeds. See scripts/speaker-ap-join.sh.
install -m 0755 "$REPO_ROOT/scripts/speaker-ap-join.sh" /usr/local/bin/speaker-ap-join.sh
install -m 0644 "$REPO_ROOT/deploy/nexus-speaker-ap-join.service" \
  /etc/systemd/system/nexus-speaker-ap-join.service
systemctl daemon-reload
systemctl enable nexus-speaker-ap-join.service

# Raise the ALSA mixer and save it.
#
# The card boots at ~78% (-19.88 dB), which is audible but noticeably weak — and on a Pi with no
# amplifier HAT, where output is already only line level from the headphone jack, that difference
# is the gap between "quiet but working" and "sounds broken". It does NOT survive a reinstall, so
# doing it by hand after every deploy is a step that will eventually be forgotten and misdiagnosed
# as an audio bug.
#
# alsactl store persists it across reboots. Failures are non-fatal: a device with no such control
# (a pure I2S DAC, for instance) should not fail the whole install over a volume default.
echo "==> Setting the output mixer level"
for ctl in PCM Master Speaker Headphone; do
  amixer -c 0 sset "$ctl" 100% unmute >/dev/null 2>&1 && echo "    $ctl -> 100%"
done
alsactl store >/dev/null 2>&1 || true

# Remind about the I2S/DAC overlay if it isn't present yet.
BOOT_CFG=/boot/firmware/config.txt
[[ -f "$BOOT_CFG" ]] || BOOT_CFG=/boot/config.txt
if [[ -f "$BOOT_CFG" ]] && ! grep -q "dtparam=i2s=on" "$BOOT_CFG"; then
  echo "==> NOTE: I2S audio overlay not detected in $BOOT_CFG"
  echo "    Append the lines from deploy/rpi/config.txt.append and reboot for audio output."
fi

echo "==> Done. Start with: systemctl start nexus-speaker && journalctl -u nexus-speaker -f"
