#pragma once

#include <string>

#include "web/LogoAsset.h"

namespace nexus::web {

// The embedded technician dashboard, served at "/". A single self-contained HTML page (no external
// assets) that polls the local API and renders device status. Kept minimal and dependency-free so
// it works offline on the speaker itself.
class Frontend {
 public:
  static std::string indexHtml() {
    return withLogo(R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Nexus Speaker</title>
<style>
  /* Design system: warm neutral greys with a single gold accent. The greys are neutral (equal
     R/G/B) so gold is the only saturated colour on screen, which is what makes an active control
     unmistakable. ok/warn/alert are STATE colours only and never decoration, so seeing one always
     means something. Identical values to the Mac app's Nexus enum — the two are one product. */
  :root {
    --bg: #2B2B2B; --bar: #1F1F1F; --surface: #333333; --row: #3D3D3D; --field: #3A3A3A;
    --accent: #C9A961;
    --text: #E8E8E8; --muted: #9A9A9A; --faint: #6E6E6E; --border: rgba(255,255,255,.10);
    --ok: #6FCF7F; --warn: #E0B341; --alert: #E06C6C;
  }
  body { font-family: system-ui, sans-serif; margin: 0; background: var(--bg); color: var(--text); }
  header {
    display: flex; align-items: center; gap: 12px;
    padding: 16px 20px; background: var(--surface);
    border-bottom: 1px solid var(--border);
  }
  .logo { height: 34px; width: auto; display: block; }
  .brand { font-size: 20px; font-weight: 700; letter-spacing: .02em; }
  .brand .n { color: var(--accent); }
  .brand .sub { color: var(--muted); font-weight: 500; font-size: 14px; margin-left: 6px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 16px; padding: 20px; }
  .card { background: var(--surface); border: 1px solid var(--border); border-radius: 6px; padding: 16px; }
  .card h2 { margin: 0 0 10px; font-size: 13px; text-transform: uppercase;
    color: var(--accent); letter-spacing: .08em; }
  .row { display: flex; justify-content: space-between; padding: 5px 0; border-top: 1px solid var(--border); }
  .row:first-child { border-top: 0; }
  .k { color: var(--muted); } .v { font-weight: 600; }
</style>
</head>
<body>
<header>
  <img class="logo" src="%LOGO_SRC%" alt="Nexus">
  <span class="brand">Speaker<span class="sub">OS</span></span>
</header>
<a href="/setup" style="display:block;text-align:center;text-decoration:none;background:var(--accent);
   color:#1F1F1F;font-weight:700;font-size:16px;padding:14px;border-radius:4px;margin-bottom:14px">
   הגדרת Wi-Fi / החלפת רשת</a>
<div id="setup-card" class="card" style="display:none">
  <h2>Setup — scan to connect</h2>
  <div style="display:flex;gap:16px;align-items:center">
    <img id="setup-qr" alt="setup QR" width="160" height="160"
         style="background:#fff;border-radius:4px;padding:4px"/>
    <div>Scan with your phone to join the setup Wi-Fi, then pick your network.</div>
  </div>
</div>
<div class="grid">
  <div class="card"><h2>Device</h2><div id="device"></div></div>
  <div class="card"><h2>Audio</h2><div id="audio"></div></div>
  <div class="card"><h2>Network</h2><div id="network"></div></div>
  <div class="card"><h2>Hardware</h2><div id="hardware"></div></div>
</div>
<script>
function render(id, obj) {
  const el = document.getElementById(id);
  el.innerHTML = Object.entries(obj || {}).map(
    ([k, v]) => `<div class="row"><span class="k">${k}</span><span class="v">${v}</span></div>`
  ).join('');
}
async function refresh() {
  try {
    const s = await (await fetch('/api/status')).json();
    // Show the setup QR while unpaired/in setup mode; hide once online.
    const card = document.getElementById('setup-card');
    if (s.setup_mode) {
      const img = document.getElementById('setup-qr');
      if (!img.src) img.src = '/api/setup-qr';
      card.style.display = '';
    } else {
      card.style.display = 'none';
    }
    render('device', { device_id: s.device_id, state: s.state, version: s.software_version });
    render('audio', { volume: s.volume, muted: s.muted, eq_profile: s.eq_profile });
    const n = await (await fetch('/api/network')).json();
    render('network', n);
    const h = await (await fetch('/api/hardware')).json();
    render('hardware', h);
  } catch (e) { /* offline */ }
}
refresh(); setInterval(refresh, 3000);
</script>
</body>
</html>)HTML");
  }

  // Interactive onboarding page served at "/setup". Unlike the read-only dashboard, this drives the
  // device: it scans for Wi-Fi (GET /api/wifi/scan), lets the user pick a network + enter the
  // password, and joins it (POST /api/wifi/connect), polling status until the device is online.
  // Self-contained (no external assets) so it works over the setup hotspot before any internet.
  //
  // `auth_token` is the device's own API token, embedded so the page can authenticate its two
  // state-changing POSTs (/api/device/name, /api/wifi/connect). Without it, onboarding worked ONLY
  // over the setup hotspot: those endpoints are auth-exempt just for a visitor arriving at the AP
  // address, so a technician using the page over the LAN (a speaker on Ethernet never raises the AP)
  // got a bare 401, which the page rendered as the misleading "החיבור נכשל" — as if the Wi-Fi
  // password were wrong, when it had never been checked. Serving the token keeps the LAN exemption
  // closed (an attacker without the token still cannot repoint the network) while letting the
  // device's own page work from either side.
  static std::string setupHtml(const std::string& auth_token = "") {
    return withLogo(injectToken(R"HTML(<!doctype html>
<html lang="he" dir="rtl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Nexus — הגדרת רמקול</title>
<style>
  :root { --bg:#2B2B2B; --bar:#1F1F1F; --surface:#333333; --row:#3D3D3D; --field:#3A3A3A;
          --accent:#C9A961; --ok:#6FCF7F; --warn:#E0B341; --alert:#E06C6C;
          --text:#E8E8E8; --muted:#9A9A9A; --faint:#6E6E6E; --border:rgba(255,255,255,.10); }
  * { box-sizing: border-box; }
  body { font-family: system-ui, sans-serif; margin:0; background:var(--bg); color:var(--text);
         min-height:100vh; display:flex; justify-content:center; }
  .wrap { width:100%; max-width:440px; padding:24px 18px; }
  header { display:flex; align-items:center; gap:10px; margin-bottom:8px; }
  .logo { height:30px; width:auto; display:block; }
  .brand { font-size:20px; font-weight:700; } .brand .n { color:var(--accent); }
  .sub { color:var(--muted); font-size:14px; margin:0 0 20px; }
  .card { background:var(--surface); border:1px solid var(--border); border-radius:6px;
          padding:16px; margin-bottom:14px; }
  h2 { font-size:13px; text-transform:uppercase; letter-spacing:.06em; color:var(--accent);
       margin:0 0 12px; }
  /* The first control in a card must not add its own top margin on top of the heading's
     12px, or every headed card sits with visibly deeper padding than its neighbours. */
  .card > h2 + input, .card > h2 + select, .card > h2 + .fld { margin-top:0; }
  /* Buttons carry margin-top for stacking; the first one after a field group would otherwise
     double up against that field's own spacing. */
  .card > button:first-child { margin-top:0; }
  .net { display:flex; justify-content:space-between; align-items:center; padding:16px 14px;
         border:1px solid var(--border); border-radius:4px; margin-bottom:8px; cursor:pointer;
         background:var(--row); font-size:16px; }
  .adv-toggle { margin-top:0; }
  .net:hover, .net.sel { border-color:var(--accent); }
  .net .name { font-weight:600; } .net .sig { color:var(--muted); font-size:12px; }
  input, select { width:100%; padding:12px; border-radius:4px; border:1px solid var(--border);
          background:var(--field); color:var(--text); font-size:16px; margin-top:8px; }
  .fld { display:block; font-size:12px; color:var(--muted); margin-top:12px; }
  .hint { font-size:12px; color:var(--muted); margin-top:8px; }
  .qrcard { text-align:center; }
  .qr { width:180px; height:180px; background:#fff; border-radius:6px; padding:8px; margin:4px auto 10px; }
  .qr-addr { font-family:monospace; font-size:15px; color:var(--accent); direction:ltr;
             background:var(--field); border:1px solid var(--border); border-radius:4px; padding:8px;
             margin-bottom:6px; user-select:all; }
  button { width:100%; padding:14px; border:0; border-radius:4px; background:var(--accent);
           color:#1F1F1F; font-size:16px; font-weight:600; cursor:pointer; margin-top:14px; }
  button:disabled { opacity:.5; cursor:default; }
  .btn-ghost { background:transparent; border:1px solid var(--border); color:var(--muted); }
  .status { text-align:center; padding:10px; border-radius:4px; font-weight:600; margin-top:6px; }
  .status.ok { background:rgba(111,207,127,.12); color:var(--ok); }
  .status.err { background:rgba(224,108,108,.12); color:var(--alert); }
  .status.info { background:rgba(201,169,97,.12); color:var(--accent); }
  .hide { display:none; }

  /* ── Connection panel ──────────────────────────────────────────────────────
     A dedicated card that reports the REAL link state, so "connected" is never
     inferred from a request succeeding. The lamp turns green only after the
     device confirms an actual Wi-Fi association with an IP. */
  .conn { display:flex; align-items:center; gap:12px; }
  .lamp { width:14px; height:14px; border-radius:50%; flex:0 0 14px;
          background:var(--muted); box-shadow:0 0 0 3px rgba(154,154,154,.15); }
  .lamp.on  { background:var(--ok);    box-shadow:0 0 0 3px rgba(111,207,127,.20); }
  .lamp.bad { background:var(--alert); box-shadow:0 0 0 3px rgba(224,108,108,.20); }
  /* Pulse only while a check is genuinely in flight, so motion always means work. */
  .lamp.busy { background:var(--accent); box-shadow:0 0 0 3px rgba(201,169,97,.20);
               animation:pulse 1.1s ease-in-out infinite; }
  @keyframes pulse { 0%,100% { opacity:1 } 50% { opacity:.35 } }
  @media (prefers-reduced-motion: reduce) { .lamp.busy { animation:none } }
  .conn .txt { flex:1; min-width:0; }
  .conn .t1 { font-weight:600; font-size:15px; }
  .conn .t2 { color:var(--muted); font-size:12.5px; margin-top:2px;
              direction:ltr; text-align:right; font-family:monospace; }
</style>
</head>
<body>
<div class="wrap">
  <header><img class="logo" src="%LOGO_SRC%" alt="Nexus">
    <span class="brand">חיבור מהיר</span></header>
  <p class="sub">בחר את רשת ה-WiFi שלך והזן סיסמה.</p>

  <!-- מצב החיבור האמיתי של הרמקול. הנורית נדלקת בירוק רק אחרי שהמכשיר עצמו
       אישר חיבור Wi-Fi עם כתובת IP — לא לפי הצלחת הבקשה. -->
  <div class="card">
    <h2>מצב חיבור</h2>
    <div class="conn">
      <span id="lamp" class="lamp"></span>
      <div class="txt">
        <div class="t1" id="conn-t1">בודק…</div>
        <div class="t2" id="conn-t2"></div>
      </div>
    </div>
  </div>

  <!-- מסך אחד זורם: רשימת רשתות → סיסמה מתחת לרשת שנבחרה → כפתור אחד גדול. -->
  <div class="card">
    <h2>רשתות זמינות</h2>
    <div id="nets"><div class="status info">סורק רשתות…</div></div>

    <!-- שדה הסיסמה מופיע כאן מרגע שבוחרים רשת (לא כרטיס נפרד). -->
    <div id="pwrow" class="hide">
      <input id="psk" type="password" placeholder="סיסמת הרשת" autocomplete="off"
             onkeydown="if(event.key==='Enter')connect()">
    </div>

    <button id="connectBtn" onclick="connect()">התחבר</button>
    <div id="status"></div>

    <button class="btn-ghost" onclick="scan()">🔄 רענן רשתות</button>
  </div>

  <!-- כל ההגדרות המתקדמות מקופלות תחת מתג יחיד — לא מפריעות לרוב המשתמשים. -->
  <button class="btn-ghost adv-toggle" onclick="toggleAdvanced()">הגדרות מתקדמות</button>
  <div class="card hide" id="advbox">
    <h2>הגדרות מתקדמות</h2>
    <label class="fld">שם הרמקול (איך יופיע לסטרימר)</label>
    <input id="spkname" type="text" placeholder="לדוגמה: סלון" autocomplete="off">

    <label class="fld">רשת מוסתרת (הזנת שם ידנית)</label>
    <input id="hidden-ssid" type="text" placeholder="שם רשת שלא משדרת (SSID)" autocomplete="off"
           oninput="pickHidden()">

    <label class="fld">באנד מועדף</label>
    <select id="band">
      <option value="">אוטומטי</option>
      <option value="bg">2.4GHz (טווח רחב)</option>
      <option value="a">5GHz (מהיר)</option>
    </select>

    <label class="fld">כתובת IP סטטית (השאר ריק ל-DHCP)</label>
    <input id="static-ip" type="text" placeholder="192.168.1.50/24" autocomplete="off">
    <input id="gateway" type="text" placeholder="שער — 192.168.1.1" autocomplete="off">
    <input id="dns" type="text" placeholder="DNS — 8.8.8.8" autocomplete="off">
  </div>

  <!-- פתיחת הדף במכשיר נוסף: סריקת ה-QR פותחת ישירות את http://10.42.0.1/setup (לאחר חיבור
       ל-Nexus-Setup). עוקף לגמרי את ה-captive portal. -->
  <div class="card qrcard">
    <h2>פתיחה בטלפון נוסף</h2>
    <img class="qr" src="/api/setup-url-qr" alt="QR לפתיחת דף ההגדרה">
    <div class="qr-addr">http://10.42.0.1</div>
    <div class="hint">התחבר לרשת <b>Nexus-Setup</b> ואז סרוק את הקוד — הדפדפן ייפתח על דף זה.</div>
  </div>
</div>
<script>
let selected = null;
let isHidden = false;  // did the user type a hidden SSID instead of picking from the scan list?

// The device serves this page, so it can hand it its own API token. Needed when the page is opened
// over the LAN (a speaker on Ethernet never raises the setup AP): the onboarding POSTs are exempt
// from auth only for a visitor arriving at the hotspot address, so without this header they 401 and
// the page misreports it as a rejected Wi-Fi password. Empty over the hotspot, where it isn't needed.
const AUTH_TOKEN = '%AUTH_TOKEN%';
function postHeaders() {
  const h = {'Content-Type':'application/json'};
  if (AUTH_TOKEN) h['Authorization'] = 'Bearer ' + AUTH_TOKEN;
  return h;
}

async function scan() {
  const el = document.getElementById('nets');
  el.innerHTML = '<div class="status info">סורק רשתות…</div>';
  try {
    const r = await (await fetch('/api/wifi/scan')).json();
    const nets = (r.networks || []).sort((a,b) => (b.signal_dbm||-100) - (a.signal_dbm||-100));
    if (!nets.length) { el.innerHTML = '<div class="status err">לא נמצאו רשתות — נסה לרענן</div>'; return; }
    el.innerHTML = nets.map(n => {
      const bars = n.signal_dbm > -60 ? 'חזק' : n.signal_dbm > -75 ? 'בינוני' : 'חלש';
      return `<div class="net" onclick="pick(this,'${encodeURIComponent(n.ssid)}')">
        <span class="name">${n.ssid || '(רשת מוסתרת)'}</span><span class="sig">${bars}</span></div>`;
    }).join('');
  } catch (e) { el.innerHTML = '<div class="status err">שגיאת סריקה — נסה לרענן</div>'; }
}

// Reveal the inline password field once a network is chosen.
function showPw() { document.getElementById('pwrow').classList.remove('hide'); }

function pick(elem, ssidEnc) {
  selected = decodeURIComponent(ssidEnc);
  isHidden = false;
  document.querySelectorAll('.net').forEach(n => n.classList.remove('sel'));
  elem.classList.add('sel');
  document.getElementById('hidden-ssid').value = '';
  showPw();
  document.getElementById('psk').focus();
}

function toggleAdvanced() { document.getElementById('advbox').classList.toggle('hide'); }

// Typing a hidden SSID (in the advanced box) selects it as the target and clears the list selection.
function pickHidden() {
  const v = document.getElementById('hidden-ssid').value.trim();
  document.querySelectorAll('.net').forEach(n => n.classList.remove('sel'));
  if (v) { selected = v; isHidden = true; showPw(); }
}

async function connect() {
  const psk = document.getElementById('psk').value;
  const st = document.getElementById('status');
  const btn = document.getElementById('connectBtn');
  if (!selected) { st.className='status err'; st.textContent='בחר רשת או הזן שם רשת מוסתרת'; return; }
  btn.disabled = true;

  // First, best-effort save the speaker name (non-fatal if it fails — Wi-Fi is the important step).
  const name = document.getElementById('spkname').value.trim();
  if (name) {
    try {
      await fetch('/api/device/name', {
        method:'POST', headers: postHeaders(),
        body: JSON.stringify({ name })
      });
    } catch (e) { /* ignore — proceed to Wi-Fi */ }
  }

  const body = {
    ssid: selected,
    psk,
    hidden: isHidden,
    band: document.getElementById('band').value,
    static_ip: document.getElementById('static-ip').value.trim(),
    gateway: document.getElementById('gateway').value.trim(),
    dns: document.getElementById('dns').value.trim(),
  };
  st.className = 'status info'; st.textContent = 'מתחבר ל-' + selected + '…';
  try {
    const r = await (await fetch('/api/wifi/connect', {
      method:'POST', headers: postHeaders(),
      body: JSON.stringify(body)
    })).json();
    if (r.ok) { pollOnline(); }
    else { st.className='status err'; st.textContent = r.message || 'החיבור נכשל'; btn.disabled=false; }
  } catch (e) { st.className='status err'; st.textContent='שגיאת רשת'; btn.disabled=false; }
}

// Paint the connection lamp from the device's OWN reported state.
//   green  = the device confirms a Wi-Fi association WITH an IP
//   red    = no uplink at all
//   grey   = an uplink exists but it is not Wi-Fi (e.g. Ethernet)
// The distinction matters: a speaker on Ethernet reports connected=true, so a naive check would
// light green on a Wi-Fi attempt that actually failed and tell the customer they are done.
function paintConn(n, busy) {
  const lamp = document.getElementById('lamp');
  const t1 = document.getElementById('conn-t1');
  const t2 = document.getElementById('conn-t2');
  if (busy) { lamp.className = 'lamp busy'; t1.textContent = 'מתחבר…'; t2.textContent = ''; return false; }
  if (!n || !n.connected || !n.ip) {
    lamp.className = 'lamp bad'; t1.textContent = 'לא מחובר'; t2.textContent = '';
    return false;
  }
  if (n.mode === 'wifi') {
    lamp.className = 'lamp on';
    t1.textContent = 'מחובר ל-WiFi';
    t2.textContent = n.ip;
    return true;
  }
  // Connected, but over something other than Wi-Fi. Say so plainly rather than implying success.
  lamp.className = 'lamp';
  t1.textContent = 'מחובר בכבל רשת (לא WiFi)';
  t2.textContent = n.ip;
  return false;
}

async function refreshConn(busy) {
  try {
    const n = await (await fetch('/api/network', {cache:'no-store'})).json();
    return paintConn(n, busy);
  } catch (e) {
    // While the radio switches networks this page can briefly become unreachable — that is not
    // itself a failure, so keep the previous state instead of flashing an error.
    if (!busy) paintConn(null, false);
    return false;
  }
}

async function pollOnline() {
  const st = document.getElementById('status');
  const btn = document.getElementById('connectBtn');
  for (let i = 0; i < 20; i++) {
    await refreshConn(true);
    await new Promise(r => setTimeout(r, 1500));
    try {
      const n = await (await fetch('/api/network', {cache:'no-store'})).json();
      // Require mode === 'wifi': on a speaker with Ethernet, connected+ip alone would report
      // success for a Wi-Fi join that never happened.
      if (n.connected && n.ip && n.mode === 'wifi') {
        paintConn(n, false);
        st.className='status ok';
        st.textContent = 'מחובר! כתובת: ' + n.ip + ' — הרמקול מוכן לסטרימר.';
        btn.disabled = false;
        return;
      }
    } catch (e) { /* hotspot may drop as it switches networks — keep trying */ }
  }
  await refreshConn(false);
  st.className='status info';
  st.textContent = 'החיבור נשלח. אם ה-WiFi שלך התחלף, בדוק את הרמקול ברשת הביתית.';
  btn.disabled = false;
}

// Live re-check every 3s so the panel reflects reality even if the link changes on its own.
refreshConn(false);
setInterval(() => refreshConn(false), 3000);

scan();
</script>
</body>
</html>)HTML",
                             auth_token));
  }

 private:
  // Substitute the "%AUTH_TOKEN%" placeholder with the device's own API token, so the page can
  // authenticate its state-changing POSTs. The token is JSON-escaped because it lands inside a JS
  // string literal; an empty token yields "" and the page simply sends no Authorization header
  // (which is correct over the setup hotspot, where those endpoints are exempt anyway).
  static std::string injectToken(std::string html, const std::string& token) {
    std::string esc;
    esc.reserve(token.size());
    for (char c : token) {
      if (c == '\\' || c == '"' || c == '\'') esc += '\\';
      esc += c;
    }
    const std::string marker = "%AUTH_TOKEN%";
    for (std::size_t pos = html.find(marker); pos != std::string::npos;
         pos = html.find(marker, pos + esc.size())) {
      html.replace(pos, marker.size(), esc);
    }
    return html;
  }

  // Substitute the "%LOGO_SRC%" placeholder in a page template with the embedded Nexus logo
  // data-URI. Keeps the big base64 blob out of the page literals (it lives in LogoAsset.h) while
  // still emitting one self-contained HTML document with no external asset requests.
  static std::string withLogo(std::string html) {
    const std::string marker = "%LOGO_SRC%";
    const std::string uri = nexusLogoDataUri();
    for (std::size_t pos = html.find(marker); pos != std::string::npos;
         pos = html.find(marker, pos + uri.size())) {
      html.replace(pos, marker.size(), uri);
    }
    return html;
  }
};

}  // namespace nexus::web
