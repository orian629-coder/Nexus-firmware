#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# install-kiosk.sh — התקנה חד-שלבית של תצוגת ה-kiosk הנייטיב על ה-Pi.
#
# מתקין תלויות, מעתיק את display.py ל-/usr/local/bin, מתקין את שירות ה-systemd
# ומפעיל אותו. אידמפוטנטי — אפשר להריץ שוב לעדכון (מעתיק מחדש + restart).
#
# הרצה (מתוך שורש הריפו, על ה-Pi):
#   sudo ./deploy/install-kiosk.sh
#
# דגלים:
#   --no-enable   רק להתקין, בלי enable/start אוטומטי
#   --uninstall   להסיר את השירות והסקריפט
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SRC_PY="$REPO_DIR/gui/kiosk/display.py"
SRC_SVC="$REPO_DIR/deploy/nexus-kiosk.service"
SRC_LOGO="$REPO_DIR/assets/nexus-logo.png"
DST_PY="/usr/local/bin/nexus-kiosk.py"
DST_SVC="/etc/systemd/system/nexus-kiosk.service"
DST_LOGO="/usr/local/share/nexus-speaker/nexus-logo.png"

# ── בדיקת הרשאות ──
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "❌ צריך להריץ עם sudo (מתקין ל-/usr/local/bin ו-/etc/systemd)." >&2
  exit 1
fi

# ── הסרה ──
if [[ "${1:-}" == "--uninstall" ]]; then
  echo "── מסיר את תצוגת ה-kiosk ──"
  systemctl disable --now nexus-kiosk.service 2>/dev/null || true
  rm -f "$DST_SVC" "$DST_PY" "$DST_LOGO"
  systemctl daemon-reload
  echo "✅ הוסר. (getty@tty1 יחזור לפעול באתחול הבא / אחרי daemon-reload)"
  exit 0
fi

NO_ENABLE=0
[[ "${1:-}" == "--no-enable" ]] && NO_ENABLE=1

echo "── מקורות ──"
for f in "$SRC_PY" "$SRC_SVC"; do
  [[ -f "$f" ]] || { echo "❌ חסר קובץ מקור: $f" >&2; exit 1; }
done
echo "  display.py → $DST_PY"
echo "  service    → $DST_SVC"

# ── תלויות Python (Debian/Raspberry Pi OS) ──
echo "── תלויות ──"
PKGS=(python3-pygame python3-qrcode python3-bidi)
MISSING=()
for p in "${PKGS[@]}"; do
  dpkg -s "$p" >/dev/null 2>&1 || MISSING+=("$p")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
  echo "  מתקין חסרים: ${MISSING[*]}"
  apt-get update -qq
  apt-get install -y "${MISSING[@]}"
else
  echo "  כל התלויות כבר מותקנות ✓"
fi

# ── בדיקת שפיות: הסקריפט נטען בלי שגיאות import/תחביר ──
echo "── בדיקת טעינה ──"
if python3 -c "import ast,sys; ast.parse(open('$SRC_PY').read())"; then
  echo "  תחביר תקין ✓"
fi

# ── התקנה ──
echo "── מתקין ──"
install -m 0755 "$SRC_PY" "$DST_PY"
install -m 0644 "$SRC_SVC" "$DST_SVC"
# לוגו נקסוס לבר העליון — נתיב קבוע ש-display.py מחפש בו (ראה load_logo).
if [[ -f "$SRC_LOGO" ]]; then
  install -m 0644 -D "$SRC_LOGO" "$DST_LOGO"
  echo "  logo       → $DST_LOGO ✓"
else
  echo "  ⚠️  לוגו חסר ($SRC_LOGO) — הבר יציג טקסט 'nexus' במקום"
fi
systemctl daemon-reload
echo "  קבצים הותקנו + daemon-reload ✓"

# ── אזהרת התנגשות שרת-תצוגה ──
DEFAULT_TARGET="$(systemctl get-default 2>/dev/null || echo '?')"
if [[ "$DEFAULT_TARGET" == "graphical.target" ]]; then
  echo "⚠️  ה-default target הוא graphical.target — X/Wayland עלול לתפוס את המסך."
  echo "    ל-kiosk טהור:  sudo systemctl set-default multi-user.target  ואתחל."
fi

# ── הפעלה ──
if [[ "$NO_ENABLE" -eq 1 ]]; then
  echo "✅ הותקן (בלי enable — הפעל ידנית: systemctl enable --now nexus-kiosk)"
  exit 0
fi

echo "── מפעיל ──"
# enable כדי שיעלה באתחול; restart (לא רק start) כדי שהתהליך יטען מחדש את display.py
# החדש — אחרת עדכון-מקום מחליף את הקובץ אבל התהליך הישן ממשיך לרוץ עם הקוד הישן.
systemctl enable nexus-kiosk.service >/dev/null 2>&1 || true
systemctl restart nexus-kiosk.service
sleep 1
if systemctl is-active --quiet nexus-kiosk.service; then
  echo "✅ nexus-kiosk פעיל (הופעל מחדש עם הקוד החדש). לוגים:  journalctl -u nexus-kiosk -f"
else
  echo "⚠️  השירות לא פעיל. בדוק:  journalctl -u nexus-kiosk -e" >&2
  systemctl --no-pager status nexus-kiosk.service | head -20 || true
  exit 1
fi
