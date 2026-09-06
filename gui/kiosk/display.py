#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Nexus Speaker — תצוגה נייטיב על מסך ה-Pi (HDMI), ללא דפדפן.

מציגה את אותו מידע כמו gui/index.html (שעון, VU כפול, EQ 30 פסים + עקומה,
סטטוס מערכת, רשת, Bluetooth/סנכרון/latency, קוד QR לשיוך) — אבל כאפליקציית
Pygame שמציירת ישירות על ה-framebuffer/KMS, נטענת אוטומטית מ-systemd באתחול.

מקור נתונים: ה-REST API המקומי של הרמקול (nexus-speaker) על :8080 — polling של
/api/status, /api/hardware, /api/network, /api/audio. (הרמקול הנוכחי חושף REST,
לא MQTT; הגרסה הזו הוסבה בהתאם.) אין תלות חיצונית לשכבת הנתונים — urllib מ-stdlib.

תלויות (על ה-Pi):
    sudo apt install python3-pygame python3-qrcode python3-bidi
    (אם python3-qrcode לא זמין: pip3 install qrcode[pil] — QR לא חובה, יש fallback)

הרצה מקומית לבדיקה (חלון במקום fullscreen, מול Pi מרוחק):
    NEXUS_KIOSK_WINDOWED=1 NEXUS_API_BASE=http://10.0.0.28:8080 python3 display.py
"""

import json
import math
import os
import re
import subprocess
import sys
import time
import threading
import urllib.request
import urllib.error

import pygame

try:
    import qrcode
except ImportError:
    qrcode = None

try:
    # אלגוריתם bidi תקני (UBA) — הסידור הנכון של עברית+לטינית/מספרים.
    from bidi.algorithm import get_display as _bidi_display
except ImportError:
    _bidi_display = None


# ─────────────────────────────────────────────────────────────────────────────
# הגדרות — נטענות מ-config.json (אם קיים), עם ברירות מחדל תואמות ל-gui/config.js.
# ─────────────────────────────────────────────────────────────────────────────
def load_config():
    cfg = {
        "apiBase": "http://localhost:8080",
        "deviceId": "speaker-pi-001",
        # Empty = build the pairing QR from the speaker's own address (http://<ip>:8080), which is
        # a page that actually exists on the device. The previous default pointed at
        # nexus-audio.example.com — example.com is IANA's reserved documentation domain, so
        # scanning the QR opened a blank page. Set this to a real join portal once one exists.
        "joinBaseUrl": "",
        # Setup hotspot the device raises while unpaired/offline (see scripts/hotspot.sh). In setup
        # mode the QR encodes a WIFI: join to this AP so the phone connects and the captive portal
        # opens /setup automatically.
        "hotspotSsid": "Nexus-Setup",
        # Empty = the setup AP is open (see scripts/hotspot.sh). Drives both the WIFI: QR payload
        # and the on-screen footer; set it to re-introduce a WPA password.
        "hotspotPassword": "",
        "fps": 30,
        "pollSec": 2.0,
    }
    # מאפשרים override מקובץ JSON (אותו device_config שהרמקול משתמש בו, אם רוצים)
    for path in (
        os.environ.get("NEXUS_KIOSK_CONFIG"),
        "/etc/nexus-speaker/config.json",
        os.path.join(os.path.dirname(__file__), "kiosk_config.json"),
    ):
        if path and os.path.isfile(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                for k in ("apiBase", "deviceId", "joinBaseUrl", "hotspotSsid", "hotspotPassword"):
                    if k in data:
                        cfg[k] = data[k]
                break
            except Exception as e:
                print(f"[kiosk] אזהרה: כשל בקריאת {path}: {e}", file=sys.stderr)
    # env overrides (נוחים לבדיקה)
    cfg["apiBase"] = os.environ.get("NEXUS_API_BASE", cfg["apiBase"]).rstrip("/")
    cfg["deviceId"] = os.environ.get("NEXUS_DEVICE_ID", cfg["deviceId"])
    cfg["hotspotSsid"] = os.environ.get("NEXUS_HOTSPOT_SSID", cfg["hotspotSsid"])
    cfg["hotspotPassword"] = os.environ.get("NEXUS_HOTSPOT_PASSWORD", cfg["hotspotPassword"])
    return cfg


def wifi_qr_payload(ssid, password):
    """QR content that makes a phone offer to join a Wi-Fi network. Standard MECARD-style WIFI:
    format understood by iOS/Android camera apps. Special chars in ssid/psk are escaped per spec."""
    def esc(v):
        out = []
        for ch in str(v):
            if ch in "\\;,:\"":
                out.append("\\" + ch)
            else:
                out.append(ch)
        return "".join(out)
    # An open network must be advertised as T:nopass with no P: field — sending T:WPA for an open
    # AP makes the phone try (and fail) to join with a password.
    if not password:
        return f"WIFI:T:nopass;S:{esc(ssid)};;"
    return f"WIFI:T:WPA;S:{esc(ssid)};P:{esc(password)};;"


CFG = load_config()

# 30 תדרי ISO — זהים ל-dsp::kBandFreqs (Phase 2) ול-EQ_FREQS ב-HTML.
EQ_FREQS = [25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
            800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
            10000, 12500, 16000, 20000]
EQ_BANDS = len(EQ_FREQS)

# ── ערכת צבעים — זהה ל-enum Nexus של אפליקציית ה-Mac (mac/NexusControl/Views.swift). ──
# האפורים החמים והזהב הם מקור האמת המשותף: הרמקול, ה-Mac והדפדפן מציגים אותו מוצר.
# הערכים כאן הם בדיוק אותם hex, מומרים ל-RGB עבור Pygame.
C_BG      = (13, 13, 14)     # רקע כהה — כמו קודם
C_BAR     = (24, 24, 26)     # הבר העליון
C_PANEL   = (32, 32, 34)     # פאנלים
C_PANEL2  = (26, 26, 28)     # שקעים/שדות
C_ROW     = (40, 40, 44)     # שורות
C_LINE    = (58, 58, 62)     # קו מפריד פנימי
C_TXT     = (226, 226, 228)  # טקסט
C_MUTED   = (154, 154, 158)
C_DIM     = (110, 110, 114)
C_ACCENT  = (201, 169, 97)   # 0xC9A961  accent    — זהב
C_ACC_DIM = (138, 116, 64)   # 0x8A7440  accentDim — זהב במנוחה (עקומת ה-EQ)
C_BORDER  = (78, 78, 82)     # מסגרות הפאנלים — אפור
C_LED     = (120, 220, 130)  # מדי סיגנל — ירוק, קריאוּת מדידה
C_LED_DIM = (40, 60, 44)     # ירוק כבוי — שקע הסגמנט
C_LED_HI  = (255, 255, 255)
C_OK      = (111, 207, 127)  # 0x6FCF7F  ok
C_WARN    = (224, 179, 65)   # 0xE0B341  warn
C_ALERT   = (224, 108, 108)  # 0xE06C6C  alert
C_WHITE   = (255, 255, 255)


# ─────────────────────────────────────────────────────────────────────────────
# State — מעודכן ע"י thread ה-MQTT, נקרא ע"י לולאת הציור. מוגן ב-lock.
# ─────────────────────────────────────────────────────────────────────────────
class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.connected = False
        self.temp = None          # °C
        self.cpu = None           # %
        self.net_self = "—"
        self.net_source = "—"
        self.playing = False
        self.state = ""           # מצב מכונת המצבים כפי שדווח ב-/api/status
        self.emergency = False
        self.muted = False
        self.volume = 50          # 0..100
        self.level = 0.0          # 0..1 (יציאה)
        self.input_volume = 50
        self.input_level = 0.0    # 0..1 (כניסה)
        self.input_level_pct = 0
        self.eq = [0.0] * EQ_BANDS
        self.bt_text = "מוכן לשיוך"
        self.sync_text = "מאזין (UDP :50005)"
        self.lat_text = "טרם נמדד"
        self.mic_level = 0.0       # רמת מיקרופון הכיול (ה-Mac), 0..1
        self.mic_active = False    # האם מגיעה רמת מיק' חיה (כיול בעיצומו)
        self.link_quality = 0.0    # איכות הקישוריות לרשת/סטרימר, 0..1 (מ-ping לשער)
        self.link_rtt_ms = None    # זמן הלוך-חזור אחרון (ms), None אם אין תגובה
        self.link_up = False       # האם השער עונה בכלל
        # ── עוצמת הסטרימר: RSSI אמיתי שהסטרימר מדווח (REPORT_LINK), טרי כל עוד יש דיווח.
        # אם לא טרי — נופלים ל-link_quality/RTT שנמדד מקומית (ה"גם וגם").
        self.streamer_rssi = None      # dBm מדווח (שלילי), None אם אין דיווח טרי
        self.streamer_sig_fresh = False  # האם הדיווח טרי (הסטרימר מחובר ומדווח כרגע)
        # ── עוצמת הטלפון: RSSI של חיבור ה-BLE בזמן ההתקנה (מ-/run/nexus/phone_ble.json).
        self.phone_connected = False   # האם טלפון מחובר ב-BLE כרגע
        self.phone_rssi = None         # dBm של חיבור ה-BLE (שלילי), None אם לא ידוע
        # setup_mode = "לא משויך לסטרימר עדיין" — הוא נשאר True גם אחרי שהרשת הוגדרה, כי הוא
        # מתאפס רק כששיוך מסתיים. לכן צריך גם net_ready כדי לדעת איזה שלב להציג:
        #   setup_mode + אין רשת  → מסך ה-hotspot (שני קודי QR)
        #   setup_mode + יש רשת   → מסך שיוך (ה-hotspot כבר לא משדר; ה-QR שלו מטעה)
        self.setup_mode = False    # הרמקול לא משויך
        self.net_ready = False     # יש חיבור רשת בפועל (IP) — הוגדר ב-apply_rest
        self.device_id = CFG["deviceId"]  # המזהה האמיתי מה-API (ברירת המחדל היא placeholder)
        self.last_update = None    # epoch של הודעה אחרונה

    def snapshot(self):
        with self.lock:
            return dict(self.__dict__, eq=list(self.eq))


ST = State()


# ─────────────────────────────────────────────────────────────────────────────
# REST poller — שואב מ-/api/* של הרמקול המקומי (nexus-speaker על :8080) ומעדכן
# את State. רץ ב-thread ברקע; כשלים (הרמקול לא זמין) מסמנים connected=False בלי
# להפיל את התצוגה. מחליף את שכבת ה-MQTT של speaker-app.
# ─────────────────────────────────────────────────────────────────────────────
def _get_json(url, timeout=3.0):
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def poll_once():
    """שאיבה בודדת של כל ה-endpoints וכתיבה ל-State. מחזיר True אם הרמקול ענה."""
    base = CFG["apiBase"]
    status = hardware = network = audio = None
    ok = False
    try:
        status = _get_json(f"{base}/api/status")
        ok = True
    except (urllib.error.URLError, OSError, ValueError):
        pass
    # שאר ה-endpoints — best-effort; אם /status נענה, הרמקול חי.
    if ok:
        for name, setter in (("hardware", "hardware"), ("network", "network"),
                             ("audio", "audio")):
            try:
                val = _get_json(f"{base}/api/{name}")
            except (urllib.error.URLError, OSError, ValueError):
                val = None
            if name == "hardware":
                hardware = val
            elif name == "network":
                network = val
            elif name == "audio":
                audio = val
    apply_rest(ok, status, hardware, network, audio)
    return ok


def apply_rest(connected, status, hardware, network, audio):
    """ממפה את סכמת ה-REST של nexus-speaker לשדות ה-State של התצוגה."""
    with ST.lock:
        ST.connected = connected
        if not connected:
            return

        if isinstance(status, dict):
            ST.muted = bool(status.get("muted", ST.muted))
            if status.get("volume") is not None:
                ST.volume = float(status["volume"])
            # מצב ניגון נגזר מהמצב הכללי של המכונה.
            st = status.get("state", "")
            ST.state = st            # נשמר גם כשלעצמו — הפוטר נגזר ממנו (למשל FACTORY_RESET)
            ST.playing = st in ("STREAMING", "PLAYING", "CONNECTED")
            # The real device id comes from the speaker itself; the config default
            # ("speaker-pi-001") is only a placeholder and would be shown — and encoded into the
            # pairing QR — instead of the actual id.
            if status.get("device_id"):
                ST.device_id = str(status["device_id"])
            # Bluetooth: setup_mode → חלון שיוך פתוח; אחרת מוכן.
            ST.setup_mode = bool(status.get("setup_mode"))
            if status.get("setup_mode"):
                ST.bt_text = "📲 חלון שיוך פתוח (Nexus Audio)"
            else:
                ST.bt_text = "מוכן לשיוך"

        if isinstance(audio, dict):
            if audio.get("muted") is not None:
                ST.muted = bool(audio["muted"])
            if audio.get("volume") is not None:
                ST.volume = float(audio["volume"])

        if isinstance(hardware, dict):
            streaming = bool(hardware.get("audio_streaming", False))
            ST.playing = ST.playing or streaming
            ST.sync_text = ("🟢 מנגן מסונכרן" if streaming
                            else "מאזין ל-UDP :50005")

        if isinstance(network, dict):
            if network.get("ip") is not None:
                ST.net_self = str(network["ip"])
            # Has the device actually got an uplink? Drives which onboarding screen is shown —
            # once it's on a network the setup hotspot is down and its QR would be a dead end.
            ST.net_ready = bool(network.get("connected")) and bool(network.get("ip"))
            # עוצמת הסטרימר: RSSI שהסטרימר מדווח, עם דגל טריות. אם אין דיווח או שאינו
            # טרי — מד הסטרימר נופל ל-link_quality/RTT שנמדד מקומית.
            link = network.get("streamer_link")
            if isinstance(link, dict) and link.get("fresh"):
                ST.streamer_rssi = link.get("wifi_signal_dbm")
                ST.streamer_sig_fresh = True
            else:
                ST.streamer_rssi = None
                ST.streamer_sig_fresh = False

        # "מקור הסאונד" = the paired streamer, NOT raw network link. network.connected only means
        # the Pi has an IP (e.g. via Ethernet); it says nothing about a streamer. Derive the source
        # from pairing/state so the kiosk doesn't falsely show "connected to a streamer" when the
        # device is merely on the network but still in setup mode / unpaired.
        if isinstance(status, dict):
            if status.get("setup_mode"):
                ST.net_source = "מצב התקנה — לא משויך"
            elif status.get("paired"):
                sid = status.get("streamer_id") or ""
                ST.net_source = ("🟢 משויך" + (f" · {sid}" if sid else ""))
            else:
                ST.net_source = "לא משויך"

        ST.last_update = time.time()


def start_poller():
    """מפעיל thread רקע שמושך מה-REST כל pollSec שניות."""
    def runner():
        while True:
            try:
                poll_once()
            except Exception as e:
                print(f"[kiosk] poll נכשל: {e}", file=sys.stderr)
                with ST.lock:
                    ST.connected = False
            time.sleep(float(CFG.get("pollSec", 2.0)))

    t = threading.Thread(target=runner, daemon=True)
    t.start()
    print(f"[kiosk] REST poller פעיל — {CFG['apiBase']}/api/* כל {CFG.get('pollSec', 2.0)}ש'")
    return t


# ─────────────────────────────────────────────────────────────────────────────
# מד' עוצמת קישוריות — ping לשער ברירת-המחדל (הנתיב אל הסטרימר/הרשת) וממפה את
# ה-RTT לאיכות 0..1 לתצוגת נקודות עוצמה. מקומי בלבד (בלי תלות בשרת). RTT נמוך →
# עוצמה מלאה; RTT גבוה/חבילה אבודה → עוצמה נמוכה.
# ─────────────────────────────────────────────────────────────────────────────
_RTT_RE = re.compile(r"time[=<]([\d.]+)\s*ms")


def _default_gateway():
    """כתובת שער ברירת-המחדל (IPv4), או None אם לא נמצא."""
    try:
        out = subprocess.run(["ip", "route", "show", "default"],
                             capture_output=True, text=True, timeout=3).stdout
        m = re.search(r"default via (\S+)", out)
        return m.group(1) if m else None
    except (OSError, subprocess.SubprocessError):
        return None


def ping_once(host):
    """ping בודד ל-host. מחזיר RTT ב-ms, או None אם אין תגובה."""
    try:
        r = subprocess.run(["ping", "-n", "-c", "1", "-W", "1", host],
                           capture_output=True, text=True, timeout=3)
        if r.returncode != 0:
            return None
        m = _RTT_RE.search(r.stdout)
        return float(m.group(1)) if m else None
    except (OSError, subprocess.SubprocessError):
        return None


def rssi_to_quality(rssi_dbm):
    """ממפה RSSI (dBm, שלילי) לאיכות 0..1 לתצוגת מד העוצמה. -50dBm ומעלה→מלא,
    -85dBm ומטה→מינימלי. סקאלה לינארית על הטווח השימושי של Wi-Fi/BLE."""
    if rssi_dbm is None:
        return 0.0
    hi, lo = -50.0, -85.0   # dBm: חזק → חלש
    if rssi_dbm >= hi:
        return 1.0
    if rssi_dbm <= lo:
        return 0.05
    return max(0.05, (rssi_dbm - lo) / (hi - lo))


def rtt_to_quality(rtt_ms):
    """ממפה RTT (ms) לאיכות 0..1. ≤5ms→מלא, ≥120ms→מינימלי (סקאלה לוגריתמית
    שמתאימה לתפיסת רשת מקומית: הפרש של כמה ms חשוב בקצה הנמוך)."""
    if rtt_ms is None:
        return 0.0
    lo, hi = 5.0, 120.0
    if rtt_ms <= lo:
        return 1.0
    if rtt_ms >= hi:
        return 0.05
    frac = (math.log(rtt_ms) - math.log(lo)) / (math.log(hi) - math.log(lo))
    return max(0.05, 1.0 - frac)


def start_link_monitor():
    """thread רקע שמודד את איכות הקישוריות לשער ומעדכן את State."""
    def runner():
        misses = 0
        while True:
            gw = _default_gateway()
            rtt = ping_once(gw) if gw else None
            with ST.lock:
                if rtt is None:
                    misses += 1
                    # אחרי כישלון בודד עדיין מציגים "חלש"; אחרי כמה — מנותק.
                    ST.link_up = misses < 3
                    ST.link_rtt_ms = None
                    # ריקבון הדרגתי של האיכות במקום קפיצה לאפס.
                    ST.link_quality *= 0.5 if misses < 3 else 0.0
                else:
                    misses = 0
                    ST.link_up = True
                    ST.link_rtt_ms = rtt
                    q = rtt_to_quality(rtt)
                    # החלקה: עלייה מיידית, ירידה הדרגתית (כמו VU).
                    ST.link_quality += (q - ST.link_quality) * (0.7 if q > ST.link_quality else 0.4)
            time.sleep(2.0)

    t = threading.Thread(target=runner, daemon=True)
    t.start()
    print("[kiosk] מד' קישוריות פעיל — ping לשער כל 2ש'")
    return t


# ─────────────────────────────────────────────────────────────────────────────
# מד' עוצמת הטלפון — קורא את ה-RSSI של חיבור ה-BLE שגשר ה-provisioning כותב ל-
# /run/nexus/phone_ble.json (process נפרד). הקובץ קיים רק כשגשר ה-BLE רץ; היעדרו
# או קובץ ישן (>6ש') = "אין טלפון מחובר". מקומי בלבד, בלי REST.
# ─────────────────────────────────────────────────────────────────────────────
PHONE_RSSI_PATH = os.environ.get("NEXUS_PHONE_RSSI_PATH", "/run/nexus/phone_ble.json")


def start_phone_monitor():
    """thread רקע שקורא את קובץ ה-RSSI של הטלפון (מגשר ה-BLE) ומעדכן את State."""
    def runner():
        while True:
            connected, rssi = False, None
            try:
                with open(PHONE_RSSI_PATH, "r", encoding="utf-8") as f:
                    data = json.load(f)
                # מתעלמים מקובץ ישן — גשר BLE שנפל משאיר קובץ תקוע.
                if time.time() - float(data.get("ts", 0)) <= 6.0:
                    connected = bool(data.get("connected"))
                    rssi = data.get("rssi_dbm")
            except (OSError, ValueError, TypeError):
                pass  # אין קובץ / לא תקין → אין טלפון
            with ST.lock:
                ST.phone_connected = connected
                ST.phone_rssi = rssi if connected else None
            time.sleep(2.0)

    t = threading.Thread(target=runner, daemon=True)
    t.start()
    print(f"[kiosk] מד' טלפון (BLE) פעיל — {PHONE_RSSI_PATH} כל 2ש'")
    return t


# ─────────────────────────────────────────────────────────────────────────────
# עזרי טקסט RTL — Pygame לא עושה bidi; הופכים מחרוזות עבריות ידנית.
# ─────────────────────────────────────────────────────────────────────────────
def is_rtl_char(ch):
    o = ord(ch)
    return 0x0590 <= o <= 0x05FF or 0x0600 <= o <= 0x06FF


def shape_rtl(text):
    """מסדר טקסט לסדר ויזואלי (Pygame מצייר משמאל-לימין לפי סדר התווים).
    מעדיף את python-bidi (UBA תקני); נופל להיוריסטיקה פשוטה אם לא מותקן.
    ההיוריסטיקה מספיקה לתוויות הקצרות שלנו (אין ניקוד/צירופים מורכבים)."""
    if not any(is_rtl_char(c) for c in text):
        return text
    if _bidi_display is not None:
        try:
            return _bidi_display(text)
        except Exception:
            pass  # נופלים להיוריסטיקה למטה
    tokens = []
    buf = ""
    buf_rtl = None
    for ch in text:
        r = is_rtl_char(ch)
        if ch.isspace():
            r = buf_rtl if buf_rtl is not None else False
        if buf and r != buf_rtl:
            tokens.append((buf, buf_rtl))
            buf = ""
        buf += ch
        buf_rtl = r
    if buf:
        tokens.append((buf, buf_rtl))
    out = []
    for seg, r in reversed(tokens):
        out.append(seg[::-1] if r else seg)
    return "".join(out)


# ─────────────────────────────────────────────────────────────────────────────
# ציור התצוגה.
# ─────────────────────────────────────────────────────────────────────────────
class Display:
    def __init__(self):
        pygame.init()
        pygame.mouse.set_visible(False)
        windowed = os.environ.get("NEXUS_KIOSK_WINDOWED")
        if windowed:
            self.screen = pygame.display.set_mode((1024, 768))
        else:
            self.screen = pygame.display.set_mode((0, 0), pygame.FULLSCREEN)
        pygame.display.set_caption("Nexus Speaker")
        self.W, self.H = self.screen.get_size()
        self.clock = pygame.time.Clock()
        self.tphase = 0.0
        self.eqph = 0.0
        self.spec = [0.0] * 80
        self.spec_prev = [0.0] * 80
        # רמות VU מוחלקות (עלייה מיידית, ירידה הדרגתית) — לא נתונים מזויפים.
        self.vu_out = 0.0
        self.vu_in = 0.0
        self.vu_mic = 0.0     # רמת מיקרופון הכיול (ה-Mac), ללימטר ליד ה-EQ

        # פונט — מנסים פונט שתומך עברית; נופלים לברירת מחדל.
        def load_font(size, bold=False):
            for name in ("DejaVu Sans", "FreeSans", "Noto Sans Hebrew", "Arial"):
                path = pygame.font.match_font(name, bold=bold)
                if path:
                    return pygame.font.Font(path, size)
            return pygame.font.Font(None, size)

        s = self.H / 600.0  # קנה מידה יחסי ל-600px גובה בסיס
        self.f_time  = load_font(int(40 * s), bold=False)
        self.f_brand = load_font(int(34 * s), bold=True)
        self.f_big   = load_font(int(20 * s), bold=True)
        self.f_med   = load_font(int(16 * s))
        self.f_small = load_font(int(13 * s))
        self.f_tiny  = load_font(int(10 * s))
        # Setup-mode QR codes (two-step flow, no reliance on the flaky captive portal):
        #   1) qr_setup — a WIFI: payload that joins the "Nexus-Setup" AP.
        #   2) qr_url   — the plain URL http://10.42.0.1/setup; scanning it AFTER joining opens the
        #      browser straight on the setup page. This is the reliable path.
        # qr_join is the app-pairing QR shown once the speaker is online.
        self.qr_setup = self.build_qr(wifi_qr_payload(CFG["hotspotSsid"], CFG["hotspotPassword"]))
        self.qr_url = self.build_qr("http://10.42.0.1")
        # Built lazily below from join_url(): with no join portal configured the URL depends on the
        # speaker's own IP, which isn't known until the first successful poll.
        self.qr_join = None
        self.qr_join_url = None
        self.qr_join_id = CFG["deviceId"]
        # Backward-compatible default used by older draw paths.
        self.qr_surf = self.qr_join
        # לוגו נקסוס (לבן על שקוף) — מוצג בבר העליון במקום טקסט "nexus".
        self.logo = self.load_logo(int(40 * s))

    def build_qr(self, data):
        if qrcode is None:
            return None
        try:
            q = qrcode.QRCode(border=1, box_size=3)
            q.add_data(data)
            q.make(fit=True)
            m = q.get_matrix()
            n = len(m)
            px = 3
            surf = pygame.Surface((n * px, n * px))
            surf.fill(C_WHITE)
            for y in range(n):
                for x in range(n):
                    if m[y][x]:
                        surf.fill((32, 32, 34), (x * px, y * px, px, px))
            return surf
        except Exception as e:
            print(f"[kiosk] QR נכשל: {e}", file=sys.stderr)
            return None

    def load_logo(self, height):
        """טוען את לוגו נקסוס ומקנה מידה לגובה נתון, שומר יחס. מחפש בכמה מיקומים
        (override בסביבה, ליד הריפו לפיתוח, ומיקום ההתקנה על ה-Pi). מחזיר None אם
        הקובץ חסר או pygame נכשל — אז נופלים חזרה לטקסט "nexus"."""
        here = os.path.dirname(os.path.abspath(__file__))
        candidates = [
            os.environ.get("NEXUS_LOGO_PATH"),
            os.path.join(here, "..", "..", "assets", "nexus-logo.png"),
            "/usr/local/share/nexus-speaker/nexus-logo.png",
        ]
        path = next((p for p in candidates if p and os.path.exists(p)), None)
        if path is None:
            print("[kiosk] לוגו לא נמצא — נופל לטקסט", file=sys.stderr)
            return None
        try:
            img = pygame.image.load(path).convert_alpha()
        except Exception as e:
            print(f"[kiosk] טעינת לוגו נכשלה: {e}", file=sys.stderr)
            return None
        w, h = img.get_size()
        if h <= 0:
            return None
        scaled_w = max(1, int(w * height / h))
        return pygame.transform.smoothscale(img, (scaled_w, height))

    # ---- primitives ----
    def text(self, s, font, color, x, y, anchor="topleft"):
        surf = font.render(shape_rtl(s), True, color)
        rect = surf.get_rect()
        setattr(rect, anchor, (x, y))
        self.screen.blit(surf, rect)
        return rect

    def panel(self, x, y, w, h, color=C_PANEL, border=True):
        """פאנל עם מסגרת אפורה."""
        pygame.draw.rect(self.screen, color, (x, y, w, h))
        if border:
            pygame.draw.rect(self.screen, C_BORDER, (x, y, w, h), 1)

    # ---- widgets ----
    def draw_topbar(self, snap):
        h = int(self.H * 0.13)
        self.panel(0, 0, self.W, h, C_BAR, border=False)
        pygame.draw.line(self.screen, C_BORDER, (0, h), (self.W, h), 2)

        # ── לוגו במרכז — הוא המיתוג היחיד בבר, בלי טקסט משני מסביב. ──
        if self.logo is not None:
            lr = self.logo.get_rect()
            lr.center = (self.W // 2, h // 2)
            self.screen.blit(self.logo, lr)
        else:
            self.text("nexus", self.f_brand, C_WHITE, self.W // 2, h // 2,
                      anchor="center")

        # ── מצב חיבור (ימין) — נשאר בבר, כי זה הדבר שצריך להיקרא ראשון. ──
        conn = snap["connected"]
        dot_c = C_OK if conn else C_ALERT
        conn_txt = "מחובר" if conn else "מנותק"
        cy = h // 2
        crect = self.text(conn_txt, self.f_small, C_MUTED, self.W - 24, cy,
                          anchor="midright")
        pygame.draw.circle(self.screen, dot_c, (crect.left - 12, cy), 6)
        # הסטרימר עצמו מוצג בשורת החיבור (draw_conn_row) ולא כאן — הצגה בשני
        # המקומות היא כפילות, והתא בשורה 2 נושא יותר מידע (כתובת + קישור + RTT).
        return h

    def draw_footer(self, y, h, snap):
        """שורה תחתונה: שעון קטן במרכז + פעולות (השתק · כיול · ריסט)."""
        pygame.draw.line(self.screen, C_BORDER, (0, y), (self.W, y), 1)
        mid = y + h // 2

        # שעון קטן במרכז — אזור הזמן של המכשיר כבר Asia/Jerusalem.
        now = time.localtime()
        self.text(time.strftime("%H:%M", now), self.f_small, C_TXT,
                  self.W // 2, mid, anchor="center")
        self.text(time.strftime("%d/%m", now), self.f_tiny, C_DIM,
                  self.W // 2, mid + 14, anchor="midtop")

        # פעולות — מצב בלבד (המסך אינו מגע): מוצג כדי שיהיה ברור מה פעיל.
        # "ריסט" נגזר ממכונת המצבים: FACTORY_RESET הוא המצב שה-API מדווח בזמן
        # איפוס. אין שדה ייעודי ב-/api/status, ולכן זו הנגזרת הנכונה ולא דגל מומצא.
        st = (snap.get("state") or "").upper()
        acts = [
            ("השתק", snap["muted"], C_ALERT),
            ("כיול", snap["mic_active"], C_OK),
            ("ריסט", "RESET" in st, C_WARN),
        ]
        bw = 92
        bx = 24
        for label, on, on_c in acts:
            col = on_c if on else C_DIM
            r = pygame.Rect(bx, mid - 13, bw, 26)
            pygame.draw.rect(self.screen, C_PANEL2, r)
            pygame.draw.rect(self.screen, col if on else C_BORDER, r, 1)
            self.text(label, self.f_tiny, col, r.centerx, r.centery, anchor="center")
            bx += bw + 8

    def draw_meter(self, x, y, w, h, title, quality, active, label, label_color):
        """מד עוצמה גנרי בסגנון לימטר הכיול: בר אנכי של סגמנטים שנדלקים מלמטה
        למעלה (ירוק רוב, צהוב, אדום בקצה). quality 0..1; תווית מצב מתחת. אפור/כבוי
        כש-active=False (המכשיר לא מחובר). משמש גם למד הסטרימר וגם למד הטלפון."""
        self.panel(x, y, w, h)
        self.text(title, self.f_tiny, C_MUTED, x + w - 8, y + 8, anchor="topright")
        pygame.draw.line(self.screen, C_LINE, (x, y + 26), (x + w, y + 26), 1)
        # אזור הבר (זהה במבנה ללימטר הכיול)
        bx = x + w // 2 - 9
        by = y + 34
        bw = 18
        bh = h - 58
        pygame.draw.rect(self.screen, C_PANEL2, (bx, by, bw, bh))
        segs = 20
        seg_h = bh / segs
        active_segs = round(quality * segs) if active else 0
        for i in range(segs):
            on = i < active_segs
            # אזורי צבע: ירוק רוב, צהוב, אדום בקצה העליון (קלאסי ללימטר)
            if not on:
                c = C_LED_DIM
            elif i >= segs * 0.9:
                c = C_ALERT
            elif i >= segs * 0.75:
                c = C_WARN
            else:
                c = C_LED
            yy = by + bh - (i + 1) * seg_h
            pygame.draw.rect(self.screen, c, (bx + 2, yy + 1, bw - 4, seg_h - 2))
        # תווית מצב מתחת
        self.text(label, self.f_small, label_color, x + w // 2, y + h - 16, anchor="midtop")

    def draw_streamer_signal(self, x, y, w, h, snap):
        """מד עוצמת הסטרימר — פעיל רק כשסטרימר באמת מחובר/מזרים.
        מקור ראשי: RSSI אמיתי שהסטרימר דיווח (streamer_rssi). אם אין דיווח טרי אך
        הסטרימר מזרים בפועל (playing) — נופל לגיבוי RTT מקומי. אם אין סטרימר פעיל
        בכלל → כבוי ('אין סטרימר'), כדי לא להציג עוצמת-רשת מזויפת כאילו יש סטרימר."""
        rssi = snap["streamer_rssi"]
        if snap["streamer_sig_fresh"] and rssi is not None:
            # הסטרימר מדווח RSSI אמיתי — המקור המדויק.
            quality = rssi_to_quality(rssi)
            lbl, lc = f"{rssi}dBm", (C_TXT if quality >= 0.33 else C_ALERT)
            active = True
        elif snap["playing"] and snap["link_up"]:
            # סטרימר מזרים אך לא מדווח RSSI (גרסה ישנה/ניתוק דיווח) → גיבוי RTT מקומי.
            # ה-RTT רלוונטי רק כשיש הזרמה בפועל; אחרת זו סתם קישוריות רשת, לא סטרימר.
            quality = snap["link_quality"]
            rtt = snap["link_rtt_ms"]
            lbl = f"{rtt:.0f}ms" if rtt is not None else "…"
            lc = C_TXT if quality >= 0.33 else (C_DIM if rtt is None else C_ALERT)
            active = True
        else:
            # אין סטרימר פעיל — כבוי. לא מציגים עוצמה כלל (מונע את הרושם השגוי של
            # 'סטרימר חזק' כשבפועל אין סטרימר, רק רשת).
            quality, lbl, lc, active = 0.0, "אין סטרימר", C_DIM, False
        self.draw_meter(x, y, w, h, "🎵 סטרימר", quality, active, lbl, lc)

    def draw_phone_signal(self, x, y, w, h, snap):
        """מד עוצמת הטלפון — RSSI של חיבור ה-BLE בזמן ההתקנה (מגשר ה-provisioning).
        מוצג רק כשטלפון מחובר; אחרת אפור/'—'."""
        if snap["phone_connected"]:
            rssi = snap["phone_rssi"]
            quality = rssi_to_quality(rssi)
            lbl = f"{rssi}dBm" if rssi is not None else "…"
            lc = C_TXT if (rssi is not None and quality >= 0.33) else C_DIM
            active = rssi is not None
        else:
            quality, lbl, lc, active = 0.0, "—", C_DIM, False
        self.draw_meter(x, y, w, h, "📱 טלפון", quality, active, lbl, lc)

    def draw_mic_meter(self, x, y, w, h, level, active):
        """לימטר אנכי לרמת מיקרופון הכיול (ה-Mac). מוצג ליד ה-EQ; מגיב רק כשמגיעה
        רמת מיק' חיה בזמן כיול (אחרת אפור/כבוי)."""
        self.panel(x, y, w, h)
        self.text("מיק' כיול", self.f_tiny, C_MUTED, x + w - 8, y + 8, anchor="topright")
        pygame.draw.line(self.screen, C_LINE, (x, y + 26), (x + w, y + 26), 1)
        # אזור הבר — רוחבו נגזר מרוחב הערוץ ומכוון לרוחב הפיידר ב-draw_channel
        # (w*0.30), כדי ששלושת ערוצי הווליום ומד הכיול ייראו באותו גודל בשורה.
        bw = max(24, int(w * 0.30))
        bx = x + w // 2 - bw // 2
        by = y + 34
        bh = h - 58
        pygame.draw.rect(self.screen, C_PANEL2, (bx, by, bw, bh))
        segs = 20
        seg_h = bh / segs
        active_segs = round(level * segs) if active else 0
        for i in range(segs):
            on = i < active_segs
            # אזורי צבע: ירוק רוב, צהוב, אדום בקצה העליון (קלאסי ללימטר)
            if not on:
                c = C_LED_DIM
            elif i >= segs * 0.9:
                c = C_ALERT
            elif i >= segs * 0.75:
                c = C_WARN
            else:
                c = C_LED
            yy = by + bh - (i + 1) * seg_h
            pygame.draw.rect(self.screen, c, (bx + 2, yy + 1, bw - 4, seg_h - 2))
        # תווית מצב מתחת
        lbl = f"{int(level*100)}%" if active else "—"
        self.text(lbl, self.f_small, C_TXT if active else C_DIM,
                  x + w // 2, y + h - 16, anchor="midtop")

    def draw_vu(self, x, y, w, h, level):
        segs = 16
        seg_h = h / segs
        for i in range(segs):
            on = i < round(level * segs)
            if not on:
                c = C_LED_DIM
            elif i >= segs * 0.85:
                c = C_LED_HI
            else:
                c = C_LED
            yy = y + h - (i + 1) * seg_h
            pygame.draw.rect(self.screen, c, (x, yy + 1, w, seg_h - 2))

    def draw_fader(self, x, y, w, h, volume, db_label):
        # מסילה
        rail_x = x + w // 2
        pygame.draw.rect(self.screen, C_PANEL2, (rail_x - 2, y, 4, h))
        # ידית לפי ווליום (100%=למעלה) — רוחב הידית נגזר מרוחב הפיידר.
        frac = 1.0 - max(0.0, min(100.0, volume)) / 100.0
        ky = int(y + frac * h)
        kw = max(14, int(w * 0.5))
        pygame.draw.rect(self.screen, C_ACCENT, (rail_x - kw // 2, ky - 5, kw, 10))
        pygame.draw.rect(self.screen, C_PANEL2,
                         (rail_x - kw // 2 + 3, ky - 1, kw - 6, 2))
        # תווית ה-dB יושבת על הידית עצמה (משמאלה), ולא מעל הפיידר: אחרי שהפיידר
        # הוארך לגובה מד הכיול, y-14 נפל על קו המפריד של הכותרת.
        self.text(db_label, self.f_tiny, C_MUTED, rail_x - kw // 2 - 6, ky,
                  anchor="midright")

    def vol_to_db(self, v):
        if v >= 100:
            return 6
        return round(-60 + (v / 100.0) * 66)

    def draw_channel(self, x, y, w, h, title, out_level, volume, extra=None):
        self.panel(x, y, w, h)
        self.text(title, self.f_small, C_MUTED, x + w - 12, y + 8, anchor="topright")
        pygame.draw.line(self.screen, C_LINE, (x, y + 26), (x + w, y + 26), 1)
        # גובה ומיקום זהים למד הכיול (draw_mic_meter: by=y+34, bh=h-58), כדי
        # שארבעת הערוצים בשורה יתחילו ויסתיימו באותו קו.
        body_y = y + 34
        fader_h = h - 58
        cx = x + w // 2
        # VU שמאל, פיידר מרכז, VU ימין — כל המידות נגזרות מרוחב הערוץ ולא
        # קבועות בפיקסלים, אחרת בערוץ צר המדים גולשים אל מחוץ לפאנל.
        vu_w = max(6, int(w * 0.07))
        fader_w = max(24, int(w * 0.30))
        gap = max(6, int(w * 0.06))
        vu_off = fader_w // 2 + gap + vu_w
        self.draw_vu(cx - vu_off, body_y, vu_w, fader_h, out_level)
        self.draw_vu(cx + vu_off - vu_w, body_y, vu_w, fader_h, out_level)
        db = self.vol_to_db(volume)
        db_label = ("+" if db > 0 else "") + f"{db}dB"
        self.draw_fader(cx - fader_w // 2, body_y, fader_w, fader_h, volume, db_label)
        if extra:
            # אותו מרווח כמו תווית מד הכיול (y+h-16), כדי שהטקסט לא יישב צמוד
            # לבסיס הפיידר אחרי שהפיידר הוארך לגובה מד הכיול.
            self.text(extra, self.f_tiny, C_TXT, x + w - 8, y + h - 16,
                      anchor="topright")

    def draw_status(self, x, y, w, h, snap):
        self.panel(x, y, w, h)
        self.text(f"סטטוס מערכת", self.f_small, C_MUTED, x + w - 12, y + 10,
                  anchor="topright")
        self.text(CFG["deviceId"], self.f_tiny, C_DIM, x + 12, y + 12)
        pygame.draw.line(self.screen, C_LINE, (x, y + 34), (x + w, y + 34), 1)
        rows = [
            ("מצב ניגון", "▶ מנגן" if snap["playing"] else "⏹ עצור", C_TXT),
            ("השתק", "מושתק" if snap["muted"] else "פעיל",
             C_ALERT if snap["muted"] else C_TXT),
            ("טמפרטורה", f"{snap['temp']:.1f}°C" if snap["temp"] is not None else "—°C", C_TXT),
            ("CPU", f"{snap['cpu']:.0f}%" if snap["cpu"] is not None else "—%", C_TXT),
            ("הודעת חירום", "📢 פעיל" if snap["emergency"] else "—",
             C_ALERT if snap["emergency"] else C_MUTED),
        ]
        ry = y + 44
        rh = (h - 48) / len(rows)
        for i, (lbl, val, vc) in enumerate(rows):
            yy = ry + i * rh
            self.panel(x + 4, yy, w - 8, rh - 3, C_PANEL2)
            self.text(lbl, self.f_small, C_MUTED, x + w - 14, yy + rh / 2 - 8,
                      anchor="topright")
            self.text(val, self.f_small, vc, x + 14, yy + rh / 2 - 8)

    def _freq_label(self, f):
        """תווית תדר קצרה: 25 / 1k / 12.5k."""
        if f >= 1000:
            v = f / 1000.0
            return (f"{v:.0f}k" if v == int(v) else f"{v:.1f}k")
        return f"{f:.0f}" if f == int(f) else f"{f:.1f}"

    def draw_eq(self, x, y, w, h, snap):
        """אקולייזר בסגנון מיקסר אנלוגי: 30 פיידרים אנכיים (תדר ISO לכל אחד),
        עם ידית אנלוגית וערך dB, ועקומה עדינה שמחברת את הידיות."""
        self.panel(x, y, w, h)
        self.text("אקולייזר — 30 פסים (ISO)", self.f_small, C_MUTED,
                  x + w - 12, y + 8, anchor="topright")
        pygame.draw.line(self.screen, C_LINE, (x, y + 30), (x + w, y + 30), 1)

        eq = snap["eq"]
        DBR = 12.0
        # אזור הפיידרים: משאירים שוליים לתוויות dB (שמאל) ותדר (למטה).
        gx = x + 34
        gy = y + 40
        gw = w - 48
        gh = h - 66                     # מקום לתוויות תדר מתחת
        slot = gw / EQ_BANDS            # רוחב תא לכל פס
        track_w = max(3, int(slot * 0.16))

        def dy(db):
            return gy + gh - ((db + DBR) / (2 * DBR)) * gh

        # ── קווי רשת אופקיים + תוויות dB ──
        for db in (12, 6, 0, -6, -12):
            yy = dy(db)
            if db == 0:
                col = C_LINE
            else:
                col = C_ROW
            pygame.draw.line(self.screen, col, (gx, yy), (gx + gw, yy), 1)
            lbl = f"+{db}" if db > 0 else str(db)
            self.text(lbl, self.f_tiny, C_MUTED, gx - 6, yy, anchor="midright")

        # ── עקומה עדינה שעוברת דרך הידיות (רקע, לפני הפיידרים) ──
        centers = [gx + (i + 0.5) * slot for i in range(EQ_BANDS)]
        pts = [(centers[i], dy(max(-DBR, min(DBR, eq[i])))) for i in range(EQ_BANDS)]
        if len(pts) > 1:
            pygame.draw.lines(self.screen, C_ACC_DIM, False, pts, 1)

        # ── 30 פיידרים אנלוגיים ──
        for i in range(EQ_BANDS):
            cx = centers[i]
            g = max(-DBR, min(DBR, eq[i]))
            ky = dy(g)
            # מסילה אנכית (שקע כהה)
            pygame.draw.rect(self.screen, C_PANEL2,
                             (cx - track_w // 2, gy, track_w, gh))
            pygame.draw.rect(self.screen, C_BAR,
                             (cx - track_w // 2, gy, track_w, gh), 1)
            # מילוי צבעוני מ-0dB עד הידית (ירוק להגברה, אדמדם להנחתה)
            zero_y = dy(0)
            fill_c = C_LED if g >= 0 else C_ALERT
            top = min(zero_y, ky)
            fh = abs(zero_y - ky)
            if fh > 1:
                pygame.draw.rect(self.screen, fill_c,
                                 (cx - track_w // 2 + 1, top, track_w - 2, fh))
            # ידית אנלוגית (cap) עם חריצים
            kw = max(6, int(slot * 0.42))
            kh = max(4, int(gh * 0.028))
            krect = pygame.Rect(int(cx - kw / 2), int(ky - kh / 2), kw, kh)
            pygame.draw.rect(self.screen, C_ROW, krect)
            pygame.draw.rect(self.screen, C_BAR, krect, 1)
            # קו-מרכז + חריצי אחיזה על הידית
            pygame.draw.line(self.screen, C_TXT,
                             (krect.left + 2, int(ky)), (krect.right - 2, int(ky)), 1)
            for dxo in (-2, 2):
                pygame.draw.line(self.screen, C_LINE,
                                 (int(cx + dxo), krect.top + 1),
                                 (int(cx + dxo), krect.bottom - 1), 1)
            # תווית תדר מתחת (מסובבת אם צפוף)
            flbl = self._freq_label(EQ_FREQS[i])
            surf = self.f_tiny.render(flbl, True, C_MUTED)
            if slot < 22:
                surf = pygame.transform.rotate(surf, 90)
            r = surf.get_rect(center=(int(cx), int(gy + gh + 10)))
            self.screen.blit(surf, r)

    def draw_net_cells(self, x, y, w, h, cells):
        n = len(cells)
        cw = w / n
        for i, (lbl, val) in enumerate(cells):
            cx = x + i * cw
            self.panel(cx + 1, y, cw - 2, h)
            self.text(lbl, self.f_tiny, C_MUTED, cx + cw - 12, y + 10,
                      anchor="topright")
            self.text(val, self.f_small, C_TXT, cx + cw - 12, y + 34,
                      anchor="topright")

    def draw_conn_row(self, x, y, w, h, snap):
        """Row 2 — connection data: this speaker's address, the streamer it receives from, and the
        link/Bluetooth state. Four equal cells so the row reads as one balanced band (RTL: the
        speaker's own address first, on the right).
        """
        rtt = snap.get("link_rtt_ms")
        link = ("מחובר" if snap.get("link_up") else "אין קישור")
        if snap.get("link_up") and rtt is not None:
            link += f" · {rtt:.0f}ms"
        cells = [
            ("כתובת הרמקול", snap["net_self"], C_TXT),
            ("מקבל סאונד מ (סטרימר)", snap["net_source"], C_TXT),
            ("סטטוס קישור", link, C_OK if snap.get("link_up") else C_ALERT),
            ("Bluetooth", snap["bt_text"], C_TXT),
        ]
        n = len(cells)
        cw = w / n
        for i, (lbl, val, vc) in enumerate(cells):
            cx = x + i * cw
            self.panel(cx + 1, y, cw - 2, h)
            self.text(lbl, self.f_tiny, C_MUTED, cx + cw - 12, y + 8, anchor="topright")
            self.text(str(val), self.f_small, vc, cx + cw - 12, y + 30, anchor="topright")

    def draw_join(self, x, y, w, h, snap):
        self.panel(x, y, w, h)
        setup = snap.get("setup_mode")
        if setup:
            # Two-step onboarding, no captive portal: (1) scan the Wi-Fi QR to join "Nexus-Setup",
            # (2) scan the URL QR to open http://10.42.0.1/setup in the browser. Two QR codes side by
            # side (RTL: step 1 on the right), each with a numbered caption.
            # The QR is the thing people actually scan, so it takes as much panel height as the
            # numbered caption underneath allows (caption needs ~20px below the code).
            qsz = h - 32
            edge = 12
            # One QR pinned to each side of the panel, explanatory text centered between them
            # (RTL: step 1 on the right edge, step 2 on the left edge).
            # Step 1 — join Wi-Fi (pinned to the right edge).
            x1 = x + w - qsz - edge
            if self.qr_setup:
                self.screen.blit(pygame.transform.smoothscale(self.qr_setup, (qsz, qsz)), (x1, y + 8))
            self.text("1 · התחבר ל-" + CFG["hotspotSsid"], self.f_small, C_TXT,
                      x1 + qsz // 2, y + qsz + 4, anchor="midtop")
            # Step 2 — open the page (pinned to the left edge).
            x2 = x + edge
            if self.qr_url:
                self.screen.blit(pygame.transform.smoothscale(self.qr_url, (qsz, qsz)), (x2, y + 8))
            self.text("2 · סרוק לפתיחת הדף", self.f_small, C_TXT,
                      x2 + qsz // 2, y + qsz + 4, anchor="midtop")
            # Explanatory text centered in the band between the two QRs.
            tx = (x2 + qsz + x1) // 2
            self.text("להגדרת הרמקול:", self.f_med, C_TXT, tx, y + 14, anchor="midtop")
            self.text("סרוק קוד 1 להתחברות לרשת, ואז קוד 2 לפתיחת הדף.",
                      self.f_small, C_MUTED, tx, y + 44, anchor="midtop")
            self.text("סיסמת הרשת: " + CFG.get("hotspotPassword", "nexussetup"),
                      self.f_small, C_LED, tx, y + 70, anchor="midtop")
            self.text("או ידנית בדפדפן: 10.42.0.1", self.f_tiny, C_DIM,
                      tx, y + 96, anchor="midtop")
        else:
            qr = self.ensure_join_qr(snap)
            if qr:
                qs = pygame.transform.smoothscale(qr, (h - 16, h - 16))
                self.screen.blit(qs, (x + w - (h - 8), y + 8))
            self.text("סרוק את הקוד באפליקציה כדי לשייך את הרמקול ללקוח.",
                      self.f_small, C_MUTED, x + w - (h + 8), y + 14, anchor="topright")
            self.text(self.join_url(snap), self.f_tiny, C_DIM, x + w - (h + 8), y + 40,
                      anchor="topright")
        upd = snap["last_update"]
        upd_s = ("עודכן: " + time.strftime("%H:%M:%S", time.localtime(upd))
                 if upd else "ממתין לנתונים…")
        self.text(upd_s, self.f_tiny, C_DIM, x + 14, y + h - 20)

    def join_url(self, snap):
        """URL encoded into the pairing QR.

        With a join portal configured, it's `<portal>?device=<id>`. Without one, fall back to the
        speaker's own web UI at http://<ip>:8080 — a page that actually exists. Pointing the QR at
        an unconfigured portal domain is what made scanning it open a blank page.
        """
        dev_id = snap.get("device_id") or CFG["deviceId"]
        base = CFG.get("joinBaseUrl", "")
        if base:
            return base + "?device=" + dev_id
        ip = snap.get("net_self")
        if ip and ip != "—":
            return "http://" + str(ip) + ":8080"
        return ""

    def ensure_join_qr(self, snap):
        """Return the pairing QR, rebuilding it when the URL it encodes changes.

        What the QR encodes isn't known at startup: the device id arrives from /api/status, and
        with no join portal configured the URL embeds the speaker's own IP. Both land a poll or
        two in, so the surface is built on demand rather than in __init__.
        """
        url = self.join_url(snap)
        if not url:
            return None
        if url != self.qr_join_url:
            self.qr_join = self.build_qr(url)
            self.qr_join_url = url
            self.qr_join_id = snap.get("device_id") or CFG["deviceId"]
        return self.qr_join

    def draw_pairing_screen(self, snap, top_h):
        """Shown when the speaker is ON the network but not yet paired to a streamer.

        This is the second onboarding step. The setup hotspot is already down by this point (one
        radio can't be a client and an AP at once), so the hotspot QR from draw_setup_screen would
        point at a network that no longer exists — a dead end. Here we show what is actually
        actionable: the speaker's address on the real network, its id, and the app-pairing QR.
        """
        pad = 6
        avail_y = top_h + pad
        avail_h = self.H - avail_y - pad
        self.panel(pad, avail_y, self.W - pad * 2, avail_h)

        title_y = avail_y + 24
        self.text("הרמקול מחובר לרשת", self.f_big, C_TXT, self.W // 2, title_y, anchor="midtop")
        self.text("נותר לשייך אותו לסטרימר", self.f_med, C_MUTED,
                  self.W // 2, title_y + 46, anchor="midtop")

        join_qr = self.ensure_join_qr(snap)

        # QR on the right, the details that let a human do it manually on the left (RTL).
        qr_top = title_y + 104
        foot_h = 58
        qsz = min(int(self.W * 0.28), self.H - pad - foot_h - qr_top - 34)
        qx = self.W // 2 + int(self.W * 0.16) - qsz // 2
        if join_qr:
            self.screen.blit(pygame.transform.smoothscale(join_qr, (qsz, qsz)), (qx, qr_top))
        else:
            self.panel(qx, qr_top, qsz, qsz, C_DIM)
            self.text("אין QR", self.f_small, C_TXT, qx + qsz // 2, qr_top + qsz // 2,
                      anchor="center")
        # Say where the code leads: with no join portal configured it opens the speaker's own page,
        # so "scan in the app" would be wrong.
        self.text("סרוק לפתיחת דף הרמקול" if not CFG.get("joinBaseUrl") else "סרוק באפליקציה לשיוך",
                  self.f_med, C_TXT, qx + qsz // 2, qr_top + qsz + 8, anchor="midtop")

        # Details column, right-aligned to sit just left of the QR.
        tx = qx - 40
        ty = qr_top + 6
        rows = [
            ("כתובת הרמקול", snap.get("net_self", "—"), C_LED),
            ("מזהה", snap.get("device_id") or CFG["deviceId"], C_TXT),
            ("מצב", "ממתין לשיוך", C_MUTED),
        ]
        for lbl, val, vc in rows:
            self.text(lbl, self.f_small, C_MUTED, tx, ty, anchor="topright")
            self.text(str(val), self.f_med, vc, tx, ty + 22, anchor="topright")
            ty += 62

        foot_y = self.H - pad - foot_h + 6
        self.text("או פתח בדפדפן: http://" + str(snap.get("net_self", "—")) + ":8080",
                  self.f_small, C_DIM, self.W // 2, foot_y, anchor="midtop")

    def draw_setup_screen(self, snap, top_h):
        """Full-screen onboarding layout, used instead of the dashboard while the speaker is
        unconfigured. The dashboard's stacked rows leave the join panel ~82px, which renders the
        QR codes at ~50px — too small to scan. Here the whole screen below the top bar belongs to
        the two codes, so they come out large, and the layout is mirror-symmetric about the centre:
        two equal columns, each with a QR above its numbered caption, and the shared instructions
        centred between them.
        """
        pad = 6
        avail_y = top_h + pad
        avail_h = self.H - avail_y - pad
        self.panel(pad, avail_y, self.W - pad * 2, avail_h)

        title_y = avail_y + 24
        self.text("להגדרת הרמקול", self.f_big, C_TXT, self.W // 2, title_y, anchor="midtop")
        self.text("סרוק קוד 1 להתחברות לרשת, ואז קוד 2 לפתיחת הדף",
                  self.f_med, C_MUTED, self.W // 2, title_y + 46, anchor="midtop")

        # Two mirror-symmetric columns about the screen centre. The QR size is driven by whichever
        # runs out first — the column width or the remaining height — so the codes stay square and
        # as large as the panel honestly allows.
        col_w = int(self.W * 0.30)
        cx_right = self.W // 2 + int(self.W * 0.20)   # step 1 (RTL: first = right)
        cx_left  = self.W // 2 - int(self.W * 0.20)   # step 2
        qr_top = title_y + 104
        cap_h = 34
        foot_h = 58
        qsz = min(col_w, self.H - pad - foot_h - cap_h - qr_top)

        for cx, surf, caption in (
            (cx_right, self.qr_setup, "1 · התחבר ל-" + CFG["hotspotSsid"]),
            (cx_left,  self.qr_url,   "2 · סרוק לפתיחת הדף"),
        ):
            qx = cx - qsz // 2
            if surf:
                self.screen.blit(pygame.transform.smoothscale(surf, (qsz, qsz)), (qx, qr_top))
            else:
                # Keep the layout stable (and say why it's blank) when python3-qrcode is missing.
                self.panel(qx, qr_top, qsz, qsz, C_DIM)
                self.text("אין QR", self.f_small, C_TXT, cx, qr_top + qsz // 2, anchor="center")
            self.text(caption, self.f_med, C_TXT, cx, qr_top + qsz + 8, anchor="midtop")

        # Footer: the manual fallbacks, centred so the screen stays balanced. The setup AP is open,
        # so there is no password to show — say so, rather than leaving a stale one on screen.
        foot_y = self.H - pad - foot_h + 6
        pw = CFG.get("hotspotPassword", "")
        self.text(("סיסמת הרשת: " + pw) if pw else "רשת פתוחה — אין צורך בסיסמה",
                  self.f_small, C_LED, self.W // 2, foot_y, anchor="midtop")
        self.text("או ידנית בדפדפן: 10.42.0.1", self.f_tiny, C_DIM,
                  self.W // 2, foot_y + 26, anchor="midtop")

    # ---- לולאה ----
    def run(self):
        running = True
        while running:
            for ev in pygame.event.get():
                if ev.type == pygame.QUIT:
                    running = False
                elif ev.type == pygame.KEYDOWN and ev.key in (pygame.K_ESCAPE, pygame.K_q):
                    running = False

            self.tphase += 0.08
            self.eqph += 0.05
            snap = ST.snapshot()

            self.screen.fill(C_BG)
            top_h = self.draw_topbar(snap)

            # While unconfigured the whole screen is the onboarding screen — the dashboard's
            # leftover strip renders the QR codes far too small to scan. Which of the two
            # onboarding screens depends on whether the network step is already done:
            # setup_mode stays true until PAIRING completes, so "setup_mode and on a network"
            # means step 1 is finished and the hotspot QR would point at a dead network.
            if snap.get("setup_mode"):
                if snap.get("net_ready"):
                    self.draw_pairing_screen(snap, top_h)
                else:
                    self.draw_setup_screen(snap, top_h)
                pygame.display.flip()
                self.clock.tick(CFG.get("fps", 30))
                continue

            # פריסת לנדסקייפ למסך HDMI 1024×768, ארבע שורות מלמעלה למטה:
            #   1. טופ-בר   — לוגו (ימין) · סטטוס רשת ושעון (שמאל)   [draw_topbar למעלה]
            #   2. נתוני חיבור — כתובת הרמקול · סטרימר · קישור · Bluetooth
            #   3. בקרת ווליום ולימטרים — כניסה / יציאה / כיול / ראשי, ואז ה-EQ
            #   4. QR
            # כל שורה ברוחב מלא, כך שהמסך נקרא כרצף אחיד ולא כאריחים מפוזרים.
            pad = 6
            full_w = self.W - pad * 2

            # ── שורה 2: נתוני חיבור ──
            conn_y = top_h + pad
            conn_h = int(self.H * 0.09)
            self.draw_conn_row(pad, conn_y, full_w, conn_h, snap)

            # ── שורה 3: בקרת ווליום ולימטרים ──
            # VU — רמות אמיתיות מה-heartbeat (בלי ריצוד מלאכותי), עם החלקה קלה:
            # עלייה מיידית, ירידה הדרגתית (peak-hold, כמו VU אמיתי).
            self.vu_out += (snap["level"] - self.vu_out) * (0.6 if snap["level"] > self.vu_out else 0.12)
            self.vu_in  += (snap["input_level"] - self.vu_in) * (0.6 if snap["input_level"] > self.vu_in else 0.12)
            self.vu_mic += (snap["mic_level"] - self.vu_mic) * (0.6 if snap["mic_level"] > self.vu_mic else 0.15)

            # ── פוטר ו-EQ נקבעים ראשונים, מלמטה כלפי מעלה, כדי שהאזור
            #    האמצעי (מיקסר + נתונים) יקבל בדיוק את מה שנשאר. ──
            foot_h = 40
            foot_y = self.H - foot_h

            # EQ — רצועה תחתונה ברוחב מלא, מעל הפוטר.
            eq_h = int(self.H * 0.22)
            eq_y = foot_y - eq_h - pad
            self.draw_eq(pad, eq_y, full_w, eq_h, snap)

            # ── האזור האמצעי: שתי עמודות ──
            #   שמאל  — המיקסר (4 ערוצים), הרכיב שמסתכלים עליו הכי הרבה.
            #   ימין  — שאר הנתונים: סטטוס מערכת ו-QR השיוך.
            mid_y = conn_y + conn_h + pad
            mid_h = eq_y - mid_y - pad
            left_w = int(full_w * 0.46)
            right_w = full_w - left_w - pad
            left_x = pad                      # המיקסר בשמאל המסך
            right_x = pad + left_w + pad      # שאר הנתונים בימין

            # ── שמאל: מיקסר, ארבעה ערוצים שווי-רוחב ──
            n_ch = 4
            ch_w = (left_w - pad * (n_ch - 1)) / n_ch
            # בתוך המיקסר נשמר סדר RTL: ראשי בימין ← כניסה בשמאל.
            x_main = left_x + left_w - ch_w
            x_cal  = x_main - pad - ch_w
            x_out  = x_cal  - pad - ch_w
            x_in   = x_out  - pad - ch_w
            self.draw_channel(x_main, mid_y, ch_w, mid_h, "ווליום ראשי",
                              min(1.0, self.vu_out), snap["volume"])
            self.draw_mic_meter(x_cal, mid_y, ch_w, mid_h,
                                min(1.0, self.vu_mic), snap["mic_active"])
            self.draw_channel(x_out, mid_y, ch_w, mid_h, "יציאה לרמקול",
                              min(1.0, self.vu_out), snap["volume"])
            self.draw_channel(x_in, mid_y, ch_w, mid_h, "כניסת סאונד",
                              min(1.0, self.vu_in), snap["input_volume"],
                              extra=f"מפלס נכנס  {snap['input_level_pct']}%")

            # ── ימין: סטטוס מערכת למעלה, QR השיוך מתחתיו ──
            st_h = int(mid_h * 0.55)
            self.draw_status(right_x, mid_y, right_w, st_h, snap)
            join_y = mid_y + st_h + pad
            join_h = mid_h - st_h - pad
            if join_h > 40:
                self.draw_join(right_x, join_y, right_w, join_h, snap)

            self.draw_footer(foot_y, foot_h, snap)

            pygame.display.flip()
            self.clock.tick(CFG.get("fps", 30))

        pygame.quit()


def main():
    start_poller()
    start_link_monitor()
    start_phone_monitor()
    Display().run()


if __name__ == "__main__":
    main()
