#!/usr/bin/env bash
# setup-audio.sh — provision the PipeWire audio graph for headless A2DP → I2S/analog routing.
#
# The speaker is an A2DP sink: a phone pairs and streams, and PipeWire+WirePlumber route that
# stream to the speaker's ALSA output. On a headless Pi (multi-user.target, no login session)
# this needs three things that the stock packages do NOT set up on their own:
#
#   1. PipeWire/WirePlumber run as *user* services of the speaker's service user. They only start
#      automatically without a login session if that user has "linger" enabled.
#   2. Some images (and the old nexus-audio stack) mask pipewire via ~/.config symlinks to
#      /dev/null. Those masks must be cleared or PipeWire can never start.
#   3. WirePlumber's Bluetooth monitor is gated on seat-monitoring (logind seat), which does not
#      exist under multi-user.target. Without disabling that gate, no A2DP endpoint appears and
#      phones connect then drop after a few seconds ("no audio sink").
#
# This runs as root (invoked from install.sh) and is idempotent. AUDIO_USER defaults to the
# speaker's service user so the speaker and the A2DP graph share one PipeWire instance — no
# cross-user socket issues.
set -euo pipefail

AUDIO_USER="${1:-nexus-speaker}"

if [[ $EUID -ne 0 ]]; then
  echo "setup-audio.sh must run as root" >&2
  exit 1
fi

if ! id "$AUDIO_USER" >/dev/null 2>&1; then
  echo "setup-audio.sh: user '$AUDIO_USER' does not exist yet" >&2
  exit 1
fi

UID_N="$(id -u "$AUDIO_USER")"
HOME_DIR="$(getent passwd "$AUDIO_USER" | cut -d: -f6)"
CONF_DIR="$HOME_DIR/.config/wireplumber/wireplumber.conf.d"
RUNTIME_DIR="/run/user/$UID_N"

echo "==> Provisioning PipeWire audio for '$AUDIO_USER' (uid $UID_N, home $HOME_DIR)"

# 0) The user needs a real, writable home for ~/.config (PipeWire/WirePlumber config). sysusers
#    creates the user with this home but not always the directory itself — ensure it exists.
install -d -o "$AUDIO_USER" -g "$AUDIO_USER" -m 0755 "$HOME_DIR"

# 1) Linger: user services start at boot without a login session (needed under multi-user.target).
loginctl enable-linger "$AUDIO_USER"

# 2) Clear any pipewire/wireplumber masks (symlinks to /dev/null) left by other stacks.
for svc in pipewire pipewire.socket pipewire-pulse pipewire-pulse.socket wireplumber; do
  link="$HOME_DIR/.config/systemd/user/${svc}.service"
  [[ "$svc" == *.socket ]] && link="$HOME_DIR/.config/systemd/user/${svc}"
  if [[ -L "$link" && "$(readlink "$link")" == "/dev/null" ]]; then
    echo "    unmasking $svc"
    rm -f "$link"
  fi
done

# 3) WirePlumber drop-in: run the Bluetooth monitor without a logind seat.
install -d -o "$AUDIO_USER" -g "$AUDIO_USER" -m 0755 "$CONF_DIR"
cat > "$CONF_DIR/50-nexus-bluez-headless.conf" <<'EOF'
# Nexus: enable the Bluetooth (A2DP) monitor on a headless, seat-less system.
# Without this, WirePlumber skips bluez under multi-user.target and phones drop after connecting.
monitor.bluez.seat-monitoring = false
monitor.bluez.properties = {
  # A2DP sink role only — the speaker receives audio, it never sources it.
  bluez5.roles = [ a2dp_sink ]
  bluez5.enable-sbc-xq = true
}
EOF
chown "$AUDIO_USER:$AUDIO_USER" "$CONF_DIR/50-nexus-bluez-headless.conf"

# 4) Enable + (re)start the user services. Run as the target user against its own bus.
run_user() { sudo -u "$AUDIO_USER" XDG_RUNTIME_DIR="$RUNTIME_DIR" "$@"; }

# The runtime dir exists once the user's systemd instance is up (linger triggers it). Ensure it.
if [[ ! -d "$RUNTIME_DIR" ]]; then
  echo "    waiting for user runtime dir…"
  systemctl start "user@${UID_N}.service" || true
  for _ in $(seq 1 10); do [[ -d "$RUNTIME_DIR" ]] && break; sleep 0.5; done
fi

run_user systemctl --user daemon-reload || true
run_user systemctl --user enable  pipewire.socket pipewire.service wireplumber.service pipewire-pulse.service 2>/dev/null || true
run_user systemctl --user restart pipewire.service wireplumber.service 2>/dev/null || true

# 5) Point the speaker's system service at this user's PipeWire socket, and order it after the
#    user instance so the graph exists before the speaker opens "default". A system unit can't
#    compute the uid itself, so we write the resolved value here as a drop-in.
DROPIN=/etc/systemd/system/nexus-speaker.service.d
install -d -m 0755 "$DROPIN"
cat > "$DROPIN/10-pipewire.conf" <<EOF
# Written by scripts/setup-audio.sh — routes the speaker's ALSA "default" into $AUDIO_USER's
# PipeWire graph (uid $UID_N). Do not edit by hand; re-run setup-audio.sh to regenerate.
[Unit]
After=user@${UID_N}.service
Wants=user@${UID_N}.service
[Service]
Environment=XDG_RUNTIME_DIR=${RUNTIME_DIR}
EOF
systemctl daemon-reload

echo "==> PipeWire audio provisioned. Verify after boot with:"
echo "    sudo -u $AUDIO_USER XDG_RUNTIME_DIR=$RUNTIME_DIR wpctl status"
