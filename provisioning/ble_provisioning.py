#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Nexus Speaker — BLE GATT provisioning channel.

When the speaker has no network, it advertises a BLE GATT service so a phone/streamer app can
connect *directly* (Just Works, no passkey) and push Wi-Fi + streamer credentials. This helper
implements only the BLE transport: it collects the provisioning JSON from a GATT characteristic
and forwards it verbatim to the speaker's existing TCP pairing endpoint (127.0.0.1:45455), which
does all validation / decryption / Wi-Fi join. So the crypto and pairing logic stay in the C++
app — this is a thin bridge, mirroring how the kiosk is a thin display over the REST API.

Why BLE GATT (not RFCOMM/SPP): iOS does not expose SPP to normal apps; BLE GATT is the only
cross-platform onboarding channel a phone app can drive. Pairing is "Just Works" (NoInputNoOutput
agent) so the first device to connect is accepted with no code, per the product requirement.

Protocol (one service, two characteristics):
  Service  UUID  6e40fdb0-b5a3-f393-e0a9-e50e24dcca9e
    - CredentialsRX (write)   6e40fdb1-… : the app writes the pairing JSON here (chunked; a write
                                            of the sentinel "\\n" or a full JSON object flushes it).
    - Status       (read/notify) 6e40fdb2-… : reports "waiting" / "received" / "ok" / "error:…".

Dependencies (already on Raspberry Pi OS): python3-dbus python3-gi (GLib). No external BLE lib.

Env / config:
  NEXUS_PAIRING_HOST (default 127.0.0.1)   — speaker TCP pairing endpoint host
  NEXUS_PAIRING_PORT (default 45455)       — speaker TCP pairing endpoint port
  NEXUS_BLE_ADAPTER  (default hci0)        — Bluetooth adapter
  NEXUS_BLE_NAME     (default "Nexus Setup")
"""

import json
import os
import socket
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

# ── BlueZ D-Bus constants ────────────────────────────────────────────────────
BLUEZ = "org.bluez"
DBUS_OM = "org.freedesktop.DBus.ObjectManager"
DBUS_PROP = "org.freedesktop.DBus.Properties"
GATT_MGR_IFACE = "org.bluez.GattManager1"
LE_ADV_MGR_IFACE = "org.bluez.LEAdvertisingManager1"
ADAPTER_IFACE = "org.bluez.Adapter1"
DEVICE_IFACE = "org.bluez.Device1"
GATT_SERVICE_IFACE = "org.bluez.GattService1"
GATT_CHRC_IFACE = "org.bluez.GattCharacteristic1"
LE_ADVERTISEMENT_IFACE = "org.bluez.LEAdvertisement1"
AGENT_IFACE = "org.bluez.Agent1"
AGENT_MGR_IFACE = "org.bluez.AgentManager1"

# ── Nexus provisioning UUIDs ─────────────────────────────────────────────────
SVC_UUID = "6e40fdb0-b5a3-f393-e0a9-e50e24dcca9e"
RX_UUID = "6e40fdb1-b5a3-f393-e0a9-e50e24dcca9e"
STATUS_UUID = "6e40fdb2-b5a3-f393-e0a9-e50e24dcca9e"

ADAPTER = os.environ.get("NEXUS_BLE_ADAPTER", "hci0")
BLE_NAME = os.environ.get("NEXUS_BLE_NAME", "Nexus Setup")
PAIR_HOST = os.environ.get("NEXUS_PAIRING_HOST", "127.0.0.1")
PAIR_PORT = int(os.environ.get("NEXUS_PAIRING_PORT", "45455"))

# Where we publish the connected phone's BLE RSSI for the kiosk to read. The kiosk display is a
# separate process (and the C++ app doesn't see the BLE link at all), so this file is the hand-off:
# the kiosk polls it to draw the phone signal meter during setup. A missing/stale file means "no
# phone connected". /run is tmpfs (RAM) so this never touches the SD card.
PHONE_RSSI_PATH = os.environ.get("NEXUS_PHONE_RSSI_PATH", "/run/nexus/phone_ble.json")


def log(msg):
    print(f"[ble-prov] {msg}", flush=True)


# ── Forward the collected JSON to the speaker's TCP pairing endpoint ─────────
def forward_to_pairing(raw_json):
    """Send the provisioning JSON to the C++ pairing endpoint and return (ok, message)."""
    try:
        # Sanity: it must be JSON with type=="pairing"; we don't validate crypto here (C++ does).
        obj = json.loads(raw_json)
        if obj.get("type") != "pairing":
            obj["type"] = "pairing"
            raw_json = json.dumps(obj)
    except Exception as e:
        return False, f"invalid json: {e}"

    try:
        with socket.create_connection((PAIR_HOST, PAIR_PORT), timeout=10) as s:
            s.sendall((raw_json.rstrip("\n") + "\n").encode("utf-8"))
            resp = s.recv(4096).decode("utf-8", "replace").strip()
        log(f"pairing endpoint replied: {resp}")
        try:
            r = json.loads(resp)
            return bool(r.get("ok")), r.get("message", "")
        except Exception:
            return ("\"ok\":true" in resp or '"ok": true' in resp), resp
    except OSError as e:
        return False, f"pairing endpoint unreachable: {e}"


# ── GATT characteristic base ─────────────────────────────────────────────────
class Characteristic(dbus.service.Object):
    def __init__(self, bus, index, uuid, flags, service):
        self.path = f"{service.path}/char{index}"
        self.uuid = uuid
        self.flags = flags
        self.service = service
        self.notifying = False
        dbus.service.Object.__init__(self, bus, self.path)

    def get_properties(self):
        return {GATT_CHRC_IFACE: {
            "Service": self.service.get_path(),
            "UUID": self.uuid,
            "Flags": self.flags,
        }}

    def get_path(self):
        return dbus.ObjectPath(self.path)

    @dbus.service.method(DBUS_PROP, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface != GATT_CHRC_IFACE:
            raise dbus.exceptions.DBusException("org.bluez.Error.InvalidArguments")
        return self.get_properties()[GATT_CHRC_IFACE]

    @dbus.service.signal(DBUS_PROP, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass


class StatusCharacteristic(Characteristic):
    """read/notify — reports provisioning progress to the app."""
    def __init__(self, bus, index, service):
        super().__init__(bus, index, STATUS_UUID, ["read", "notify"], service)
        self.value = b"waiting"

    def set_status(self, text):
        self.value = text.encode("utf-8")
        log(f"status → {text}")
        if self.notifying:
            self.PropertiesChanged(GATT_CHRC_IFACE,
                                   {"Value": dbus.Array(self.value, signature="y")}, [])

    @dbus.service.method(GATT_CHRC_IFACE, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        return dbus.Array(self.value, signature="y")

    @dbus.service.method(GATT_CHRC_IFACE)
    def StartNotify(self):
        self.notifying = True

    @dbus.service.method(GATT_CHRC_IFACE)
    def StopNotify(self):
        self.notifying = False


class CredentialsRxCharacteristic(Characteristic):
    """write — the app writes the pairing JSON here (chunked). Flushes on a complete JSON object."""
    def __init__(self, bus, index, service, status_chrc):
        super().__init__(bus, index, RX_UUID, ["write", "write-without-response"], service)
        self.status = status_chrc
        self.buffer = bytearray()

    @dbus.service.method(GATT_CHRC_IFACE, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        self.buffer.extend(bytes(value))
        # Try to parse what we have; a full JSON object means the app finished sending.
        try:
            text = self.buffer.decode("utf-8")
        except UnicodeDecodeError:
            return  # mid-multibyte chunk; wait for more
        text = text.strip()
        if not (text.startswith("{") and text.endswith("}")):
            return  # not a complete object yet
        try:
            json.loads(text)
        except ValueError:
            return  # still partial
        # Complete JSON received — forward it and reset.
        log(f"received {len(self.buffer)} bytes of provisioning data")
        self.status.set_status("received")
        raw = text
        self.buffer = bytearray()
        ok, msg = forward_to_pairing(raw)
        self.status.set_status("ok" if ok else f"error:{msg[:40]}")


class ProvisioningService(dbus.service.Object):
    PATH = "/org/nexus/prov/service0"

    def __init__(self, bus):
        self.path = self.PATH
        self.bus = bus
        self.uuid = SVC_UUID
        self.primary = True
        dbus.service.Object.__init__(self, bus, self.path)
        self.status = StatusCharacteristic(bus, 0, self)
        self.rx = CredentialsRxCharacteristic(bus, 1, self, self.status)
        self.chrcs = [self.status, self.rx]

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def get_properties(self):
        return {GATT_SERVICE_IFACE: {"UUID": self.uuid, "Primary": self.primary}}

    @dbus.service.method(DBUS_PROP, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface != GATT_SERVICE_IFACE:
            raise dbus.exceptions.DBusException("org.bluez.Error.InvalidArguments")
        return self.get_properties()[GATT_SERVICE_IFACE]


class Application(dbus.service.Object):
    """Root ObjectManager exposing the service + characteristics to BlueZ."""
    def __init__(self, bus):
        self.path = "/org/nexus/prov"
        self.bus = bus
        self.service = ProvisioningService(bus)
        dbus.service.Object.__init__(self, bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    @dbus.service.method(DBUS_OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        out = {self.service.get_path(): self.service.get_properties()}
        for c in self.service.chrcs:
            out[c.get_path()] = c.get_properties()
        return out


# ── LE advertisement (so the phone can discover us) ──────────────────────────
class Advertisement(dbus.service.Object):
    PATH = "/org/nexus/prov/adv0"

    def __init__(self, bus):
        dbus.service.Object.__init__(self, bus, self.PATH)

    def get_path(self):
        return dbus.ObjectPath(self.PATH)

    @dbus.service.method(DBUS_PROP, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface != LE_ADVERTISEMENT_IFACE:
            raise dbus.exceptions.DBusException("org.bluez.Error.InvalidArguments")
        # Minimal, valid payload: peripheral type + the 128-bit service UUID. The name goes in the
        # scan response automatically; keeping the primary packet lean avoids the 31-byte overflow.
        return dbus.Dictionary({
            "Type": dbus.String("peripheral"),
            "ServiceUUIDs": dbus.Array([SVC_UUID], signature="s"),
            "LocalName": dbus.String(BLE_NAME),
        }, signature="sv")

    @dbus.service.method(LE_ADVERTISEMENT_IFACE, in_signature="", out_signature="")
    def Release(self):
        log("advertisement released")


# ── Just Works pairing agent (accept the first device, no passkey) ───────────
class JustWorksAgent(dbus.service.Object):
    PATH = "/org/nexus/prov/agent"

    def __init__(self, bus, path):
        dbus.service.Object.__init__(self, bus, path)

    @dbus.service.method(AGENT_IFACE, in_signature="", out_signature="")
    def Release(self):
        pass

    @dbus.service.method(AGENT_IFACE, in_signature="os", out_signature="")
    def AuthorizeService(self, device, uuid):
        log(f"auto-authorize service {uuid} for {device}")  # accept

    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="")
    def RequestAuthorization(self, device):
        log(f"auto-authorize device {device}")  # accept

    @dbus.service.method(AGENT_IFACE, in_signature="ou", out_signature="")
    def RequestConfirmation(self, device, passkey):
        log(f"auto-confirm {device} (Just Works)")  # accept without showing a code

    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="s")
    def RequestPinCode(self, device):
        return "0000"

    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="u")
    def RequestPasskey(self, device):
        return dbus.UInt32(0)

    @dbus.service.method(AGENT_IFACE, in_signature="", out_signature="")
    def Cancel(self):
        pass


def find_adapter_path(bus):
    om = dbus.Interface(bus.get_object(BLUEZ, "/"), DBUS_OM)
    for path, ifaces in om.GetManagedObjects().items():
        if GATT_MGR_IFACE in ifaces and path.endswith(ADAPTER):
            return path
    raise RuntimeError(f"adapter {ADAPTER} with GATT support not found")


# ── Phone RSSI publisher ─────────────────────────────────────────────────────
def _connected_device_rssi(bus, adapter_path):
    """RSSI (dBm, negative) of the phone currently connected to our adapter, or None if none is
    connected / RSSI unknown. Walks BlueZ's managed objects for a Device1 under this adapter with
    Connected==True and reads its RSSI property. BlueZ only populates RSSI while it has recent
    advertising/link data for the device; a connected-but-quiet device may briefly report None."""
    try:
        om = dbus.Interface(bus.get_object(BLUEZ, "/"), DBUS_OM)
        objects = om.GetManagedObjects()
    except dbus.exceptions.DBusException:
        return None
    for path, ifaces in objects.items():
        dev = ifaces.get(DEVICE_IFACE)
        if dev is None:
            continue
        # Only devices under our adapter (path is <adapter_path>/dev_XX_XX_...).
        if not path.startswith(adapter_path + "/"):
            continue
        if not bool(dev.get("Connected", False)):
            continue
        rssi = dev.get("RSSI")
        if rssi is None:
            return None
        try:
            return int(rssi)
        except (TypeError, ValueError):
            return None
    return None


def publish_phone_rssi(bus, adapter_path):
    """Poll the connected phone's BLE RSSI and write it (atomically) to PHONE_RSSI_PATH so the kiosk
    can draw the phone signal meter. Called on a GLib timer. Returns True to stay scheduled."""
    rssi = _connected_device_rssi(bus, adapter_path)
    try:
        os.makedirs(os.path.dirname(PHONE_RSSI_PATH), exist_ok=True)
        payload = {"connected": rssi is not None, "rssi_dbm": rssi, "ts": int(time.time())}
        tmp = PHONE_RSSI_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(payload, f)
        os.replace(tmp, PHONE_RSSI_PATH)  # atomic swap so the kiosk never reads a half-written file
    except OSError as e:
        log(f"phone RSSI publish failed: {e}")
    return True  # keep the GLib timeout registered


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    adapter_path = find_adapter_path(bus)
    adapter = bus.get_object(BLUEZ, adapter_path)
    props = dbus.Interface(adapter, DBUS_PROP)

    # Power on + pairable. Do NOT set Adapter1.Discoverable: that turns on BR/EDR inquiry-scan,
    # which on the Pi's Broadcom controller conflicts with registering an LE advertisement and makes
    # RegisterAdvertisement fail with "Invalid Parameters (0x0d)". LE discovery is handled entirely
    # by the advertisement itself, so BR/EDR discoverability isn't needed here.
    props.Set(ADAPTER_IFACE, "Powered", dbus.Boolean(True))
    props.Set(ADAPTER_IFACE, "Discoverable", dbus.Boolean(False))
    props.Set(ADAPTER_IFACE, "Pairable", dbus.Boolean(True))
    props.Set(ADAPTER_IFACE, "Alias", dbus.String(BLE_NAME))

    # Just Works agent (accept the first device with no passkey).
    agent = JustWorksAgent(bus, JustWorksAgent.PATH)
    agent_mgr = dbus.Interface(bus.get_object(BLUEZ, "/org/bluez"), AGENT_MGR_IFACE)
    agent_mgr.RegisterAgent(JustWorksAgent.PATH, "NoInputNoOutput")
    agent_mgr.RequestDefaultAgent(JustWorksAgent.PATH)

    # Register the GATT application + LE advertisement over D-Bus.
    app = Application(bus)
    gatt_mgr = dbus.Interface(bus.get_object(BLUEZ, adapter_path), GATT_MGR_IFACE)
    adv = Advertisement(bus)
    adv_mgr = dbus.Interface(bus.get_object(BLUEZ, adapter_path), LE_ADV_MGR_IFACE)

    loop = GLib.MainLoop()

    def on_reg_ok(what):
        log(f"{what} registered")

    def on_reg_err(what, e):
        log(f"ERROR registering {what}: {e}")
        loop.quit()

    gatt_mgr.RegisterApplication(app.get_path(), {},
                                 reply_handler=lambda: on_reg_ok("GATT app"),
                                 error_handler=lambda e: on_reg_err("GATT app", e))
    adv_mgr.RegisterAdvertisement(adv.get_path(), {},
                                  reply_handler=lambda: on_reg_ok("advertisement"),
                                  error_handler=lambda e: on_reg_err("advertisement", e))

    # Publish the connected phone's BLE RSSI every 2 s so the kiosk can show a phone signal meter
    # during setup. Runs on the GLib loop (no extra thread); writes PHONE_RSSI_PATH atomically.
    publish_phone_rssi(bus, adapter_path)  # write an initial "no phone" state immediately
    GLib.timeout_add_seconds(2, publish_phone_rssi, bus, adapter_path)

    log(f"BLE provisioning coming up as '{BLE_NAME}' on {ADAPTER}; "
        f"forwarding to {PAIR_HOST}:{PAIR_PORT}. Service {SVC_UUID}. "
        f"phone RSSI → {PHONE_RSSI_PATH}")
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            adv_mgr.UnregisterAdvertisement(adv.get_path())
            gatt_mgr.UnregisterApplication(app.get_path())
        except Exception:
            pass
        log("stopped")


if __name__ == "__main__":
    sys.exit(main())
