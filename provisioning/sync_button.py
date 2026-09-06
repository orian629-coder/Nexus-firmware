#!/usr/bin/env python3
"""Sync button: hold a trigger for 5 seconds to (re)enter setup mode so the speaker can be joined
to Wi-Fi through its own web interface (the hotspot + /setup captive portal).

This is the physical escape hatch for a headless speaker with no screen: once it's booted (or even
paired), there is otherwise no way to reopen the Wi-Fi setup. Holding the trigger brings the setup
channel back up on demand.

Trigger layer is pluggable so the same action works for two inputs:
  • keyboard  — SPACE held 5s on a USB keyboard (reads /dev/input/event* directly, no evdev dep)
  • gpio      — a momentary button on a GPIO pin held 5s (uses gpiozero)

Action layer is a single function enter_sync_mode(): it starts the Wi-Fi setup hotspot and reopens
a fresh setup window on the speaker, then the phone joins "Nexus-Setup" and picks a network at
http://10.42.0.1/setup.

Env:
  NEXUS_SYNC_INPUT     keyboard | gpio           (default keyboard)
  NEXUS_SYNC_HOLD_SEC  hold duration in seconds  (default 5)
  NEXUS_SYNC_KBD       input device path/glob    (default auto-detect *-event-kbd)
  NEXUS_SYNC_GPIO_PIN  BCM pin number for gpio   (default 17)
"""
import glob
import os
import struct
import subprocess
import sys
import time

HOLD_SEC = float(os.environ.get("NEXUS_SYNC_HOLD_SEC", "5"))
INPUT = os.environ.get("NEXUS_SYNC_INPUT", "keyboard")

# Linux input_event: struct timeval (long sec, long usec) + u16 type + u16 code + s32 value.
# On 64-bit (aarch64) longs are 8 bytes -> format 'llHHi', size 24.
_EV_FORMAT = "llHHi"
_EV_SIZE = struct.calcsize(_EV_FORMAT)
_EV_KEY = 0x01          # event type: key
_KEY_SPACE = 57         # Linux keycode for SPACE
_VAL_DOWN, _VAL_UP = 1, 0


def log(msg):
    print(f"[sync-button] {msg}", flush=True)


# ── Action: (re)enter setup mode so the phone can join it to Wi-Fi ──
def enter_sync_mode():
    log("SYNC held — entering setup mode (hotspot + web setup)")
    # Restart the speaker to open a fresh setup window (setup code + beginSetupMode run at start),
    # and ensure the Wi-Fi setup hotspot is up so a phone can reach http://10.42.0.1/setup.
    cmds = [
        ["systemctl", "restart", "nexus-speaker.service"],
        ["systemctl", "start", "nexus-hotspot.service"],
    ]
    for c in cmds:
        try:
            subprocess.run(c, check=False, timeout=20)
            log("ran: " + " ".join(c))
        except Exception as e:  # noqa: BLE001 - best effort, never crash the button
            log(f"warn: {' '.join(c)} failed: {e}")
    log("setup mode requested — join Wi-Fi 'Nexus-Setup' then open http://10.42.0.1/setup")


# ── Trigger: keyboard SPACE held HOLD_SEC (watch ALL keyboards) ──
def find_keyboards():
    """Return every keyboard event device, so a SPACE hold works on whichever keyboard is used.
    Honors NEXUS_SYNC_KBD (single path) as an override."""
    env = os.environ.get("NEXUS_SYNC_KBD")
    if env and os.path.exists(env):
        return [env]
    kbds = [os.path.realpath(p) for p in glob.glob("/dev/input/by-id/*-event-kbd")]
    if kbds:
        # de-dup while preserving order
        seen, out = set(), []
        for k in kbds:
            if k not in seen:
                seen.add(k); out.append(k)
        return out
    evs = sorted(glob.glob("/dev/input/event*"))
    return evs[:1]


def _watch_one(dev):
    """Read one keyboard device forever; fire enter_sync_mode when SPACE is held HOLD_SEC."""
    held_since = None
    fired = False
    try:
        with open(dev, "rb") as f:
            while True:
                data = f.read(_EV_SIZE)
                if not data or len(data) < _EV_SIZE:
                    continue
                _s, _us, etype, code, value = struct.unpack(_EV_FORMAT, data)
                if etype != _EV_KEY or code != _KEY_SPACE:
                    continue
                if value == _VAL_DOWN and held_since is None:
                    held_since = time.time()
                    fired = False
                elif value == _VAL_UP:
                    held_since = None
                    fired = False
                if held_since is not None and not fired:
                    if time.time() - held_since >= HOLD_SEC:
                        fired = True
                        enter_sync_mode()
    except Exception as e:  # noqa: BLE001 - one bad device must not kill the others
        log(f"watch {dev} ended: {e}")


def run_keyboard():
    import threading

    devs = find_keyboards()
    if not devs:
        log("no keyboard input device found")
        sys.exit(1)
    log(f"listening on {len(devs)} keyboard(s): {', '.join(devs)} — hold SPACE {HOLD_SEC:.0f}s")
    threads = [threading.Thread(target=_watch_one, args=(d,), daemon=True) for d in devs]
    for t in threads:
        t.start()
    # Keep the process alive while the watcher threads run.
    for t in threads:
        t.join()


# ── Trigger: GPIO momentary button held HOLD_SEC (for later, wired to a Pi pin) ──
def run_gpio():
    from gpiozero import Button  # imported lazily so keyboard mode needs no gpiozero

    pin = int(os.environ.get("NEXUS_SYNC_GPIO_PIN", "17"))
    log(f"GPIO button on BCM{pin} — hold {HOLD_SEC:.0f}s to enter setup mode")
    # hold_time triggers when_held after the button is held that long.
    btn = Button(pin, hold_time=HOLD_SEC)
    btn.when_held = enter_sync_mode
    from signal import pause
    pause()


def main():
    if INPUT == "gpio":
        run_gpio()
    else:
        run_keyboard()


if __name__ == "__main__":
    main()
