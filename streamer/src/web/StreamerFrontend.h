#pragma once

#include <string>

#include "web/LogoAsset.h"

namespace nexus::streamer::web {

// Self-contained control UI served at "/". No external assets (offline-capable), RTL Hebrew, Nexus
// palette matching the speaker's Frontend.h. Talks to the streamer's own /api/* endpoints, which
// forward signed commands to the selected speaker. Kept as one raw-string constant so the streamer
// binary needs no filesystem assets — same approach as the speaker dashboard.
inline constexpr const char* kControlUiTemplate = R"HTML(<!DOCTYPE html>
<html lang="he" dir="rtl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Nexus — שלט סטרימר</title>
<style>
  /* The product line: warm greys + gold, identical to the speaker's two pages and the Mac app's
     Nexus enum. 774fbe8 moved the speaker off the old purple/blue palette but missed this page, so
     the controller looked like a different product from the speakers it controls. */
  :root { --bg:#2B2B2B; --bar:#1F1F1F; --surface:#333333; --row:#3D3D3D; --field:#3A3A3A;
          --accent:#C9A961; --ok:#6FCF7F; --warn:#E0B341; --alert:#E06C6C;
          --text:#E8E8E8; --muted:#9A9A9A; --faint:#6E6E6E; --border:rgba(255,255,255,.10); }
  * { box-sizing:border-box; }
  body { font-family:system-ui,sans-serif; margin:0; background:var(--bg); color:var(--text);
         min-height:100vh; display:flex; justify-content:center; }
  .wrap { width:100%; max-width:560px; padding:24px 18px 60px; }
  header { display:flex; align-items:center; gap:10px; margin-bottom:4px; }
  /* The real Nexus mark, as on the speaker's pages — not an abstract glowing square. */
  .logo { height:30px; width:auto; display:block; }
  .brand { font-size:20px; font-weight:700; } .brand .n { color:var(--accent); }
  .sub { color:var(--muted); font-size:14px; margin:0 0 18px; }
  .card { background:var(--surface); border:1px solid var(--border); border-radius:14px;
          padding:16px; margin-bottom:14px; }
  /* ── Everyday use vs installation ──────────────────────────────────────────
     The page opened with four technical panels (source/routing, master DSP, distance
     measurement, network scan) before the customer reached their own speakers. Those are set up
     once and then never touched, so they now live behind a collapsed section and the speakers come
     first. Nothing is removed — <details> keeps it all one click away. */
  details.advanced { margin-top:22px; border-top:1px solid var(--border); padding-top:16px; }
  details.advanced > summary { cursor:pointer; list-style:none; color:var(--muted);
                               font-size:14px; padding:10px 2px; user-select:none; }
  details.advanced > summary::-webkit-details-marker { display:none; }
  details.advanced > summary::before { content:'▸ '; color:var(--faint); }
  details.advanced[open] > summary::before { content:'▾ '; }
  details.advanced > summary:hover { color:var(--text); }
  /* A short line under a section title, explaining in plain words what it is for. The UI was
     written for whoever built it; a customer needs to be told what a control does. */
  .hint { color:var(--muted); font-size:13px; margin:-2px 0 12px; line-height:1.5; }
  /* The section title sits ON the card's top border, so it needs its own line and bottom margin —
     previously it could collide with the first control in the card. */
  h2 { font-size:13px; text-transform:uppercase; letter-spacing:.06em; color:var(--accent);
       margin:0 0 14px; padding-bottom:10px; border-bottom:1px solid var(--border);
       display:flex; justify-content:space-between; align-items:baseline; gap:8px; }
  h2 .h2sub { text-transform:none; letter-spacing:0; color:var(--muted); font-weight:400;
              font-size:12px; }
  .spk { display:flex; justify-content:space-between; align-items:center; padding:12px;
         border:1px solid var(--border); border-radius:10px; margin-bottom:8px; cursor:pointer;
         background:#12141d; gap:10px; }
  .spk.sel { border-color:var(--accent); box-shadow:0 0 0 1px var(--accent) inset; }
  .spk .name { font-weight:600; } .spk .meta { color:var(--muted); font-size:12px; }
  .tel { font-size:11px; color:var(--muted); margin-top:3px; direction:ltr; text-align:right; }
  .tel .good { color:var(--ok); } .tel .warn { color:var(--warn); } .tel .bad { color:var(--alert); }
  .tel .muted-tel, .muted-tel { opacity:.6; font-style:italic; }
  .mtab { width:100%; border-collapse:collapse; font-size:12.5px; margin-top:4px; }
  .mtab th { text-align:right; color:var(--muted); font-weight:500; font-size:11px;
             border-bottom:1px solid var(--border); padding:6px 4px; }
  .mtab td { padding:6px 4px; border-bottom:1px solid rgba(255,255,255,.04); }
  .mtab .good { color:var(--ok); } .mtab .warn { color:var(--warn); } .mtab .bad { color:var(--alert); }
  .dot { width:9px; height:9px; border-radius:50%; display:inline-block; margin-inline-start:6px; }
  .dot.on { background:var(--ok); } .dot.off { background:var(--alert); }
  input,select { width:100%; padding:11px; border-radius:10px; border:1px solid var(--border);
          background:#0c0e14; color:var(--text); font-size:15px; }
  input[type=range] { padding:0; }
  .row { display:flex; gap:8px; align-items:center; } .row > * { flex:1; }
  .field { margin-top:12px; }
  button { padding:12px; border:0; border-radius:10px; background:var(--accent); color:#fff;
           font-size:15px; font-weight:600; cursor:pointer; }
  button.ghost { background:#12141d; border:1px solid var(--border); color:var(--text); }
  button:disabled { opacity:.5; cursor:default; }
  .transport { display:flex; gap:8px; }
  /* Label and its value on one line, without float — float was what made the value text overlap
     the control's border on narrow screens. */
  .lbl { display:flex; justify-content:space-between; align-items:baseline; gap:8px;
         font-size:12px; color:var(--muted); margin-bottom:6px; }
  .lbl .val { color:var(--accent); font-weight:600; font-variant-numeric:tabular-nums; }
  .status { font-size:13px; color:var(--muted); margin-top:10px; min-height:18px; }
  .muted { color:var(--muted); font-size:13px; }
  .grid2 { display:grid; grid-template-columns:1fr 1fr; gap:8px; }
  #devout { background:#0c0e14; border:1px solid var(--border); border-radius:8px; padding:10px;
            font-size:12.5px; color:var(--muted); white-space:pre-wrap; margin-top:8px;
            direction:ltr; text-align:right; min-height:1em; }
  .danger { margin-top:16px; padding-top:12px; border-top:1px solid var(--border);
            display:grid; grid-template-columns:1fr 1fr; gap:8px; }
  .danger button { color:var(--alert); border-color:rgba(255,82,82,.3); }

  /* ── VU meters ── */
  .meter { margin-bottom:10px; }
  .meter:last-child { margin-bottom:0; }
  .mrow { display:flex; align-items:center; gap:8px; margin-bottom:4px; }
  .mlabel { font-size:12px; color:var(--muted); width:52px; flex:none; }
  .mbar { flex:1; height:9px; background:#0c0e14; border:1px solid var(--border);
          border-radius:5px; overflow:hidden; position:relative; }
  .mfill { height:100%; width:0%; border-radius:4px;
           background:linear-gradient(90deg,var(--ok) 0%,var(--ok) 65%,var(--warn) 85%,var(--alert) 100%);
           background-size:calc(100% * (1 / max(var(--f,0.001), 0.001))) 100%;
           transition:width .08s linear; }
  .mdb { font-size:11px; color:var(--muted); width:56px; flex:none; text-align:left;
         direction:ltr; font-variant-numeric:tabular-nums; }
  .clip { color:var(--alert); font-weight:700; }
  .sigstate { font-size:12px; color:var(--muted); }

  /* ── EQ ── */
  .eqwrap { display:flex; gap:3px; align-items:flex-end; height:110px; margin-top:6px;
            overflow-x:auto; padding-bottom:4px; }
  .eqband { display:flex; flex-direction:column; align-items:center; gap:3px; flex:none; }
  .eqband input[type=range] { writing-mode:vertical-lr; direction:rtl; width:14px; height:80px;
                              padding:0; background:transparent; border:0; }
  .eqband .f { font-size:8px; color:var(--muted); direction:ltr; }
</style>
</head>
<body>
<div class="wrap">
  <header><img class="logo" src="%LOGO_SRC%" alt="Nexus"><span class="brand"><span class="n">Nexus</span> שלט</span></header>
  <p class="sub">שליטה על הרמקולים דרך הסטרימר</p>



  <div class="card">
    <h2>הרמקולים שלי</h2>
    <p class="hint">בחר רמקול כדי לשלוט בעוצמה, בהשתקה ובצליל שלו.</p>
    <div id="speakers"><p class="muted">אין רמקולים עדיין — הוסף רמקול חדש למטה.</p></div>
    <div class="row" style="margin-top:10px">
      <input id="add-id" placeholder="מזהה (SPK-XXXX)">
      <input id="add-name" placeholder="שם (סלון)">
    </div>
    <div class="field"><input id="add-host" placeholder="כתובת IP (10.0.0.50)"></div>
    <button class="ghost" onclick="addSpeaker()" style="margin-top:10px; width:100%">הוסף ידנית (כבר מזווג)</button>
  </div>

  <div class="card">
    <h2>פתיחה בטלפון</h2>
    <p class="hint">סרוק כדי לפתוח את הממשק בטלפון — הוא ייפתח מחובר, בלי להקליד כתובת או סיסמה.</p>
    <p class="muted" style="color:#E0B341">⚠ הקוד מכיל את מפתח הגישה. כל מי שמצלם אותו מקבל שליטה מלאה.</p>
    <div style="text-align:center; margin-top:10px">
      <!-- Loaded via fetch (see loadControlQr), NOT a bare src=: the browser sends no Authorization
           header on an <img> request, and /api/control-qr requires the token like every /api route,
           so a plain src= would render a broken image. -->
      <img id="control-qr" alt="QR לפתיחת ממשק הניהול"
           style="width:200px; height:200px; background:#fff; padding:8px; border-radius:8px; display:none">
      <p class="muted" id="control-qr-msg">טוען…</p>
    </div>
  </div>

  <div class="card">
    <h2>הוספת רמקול חדש</h2>
    <p class="hint">הדלק את הרמקול החדש ולחץ חיפוש. בחר אותו מהרשימה, הזן את פרטי ה-WiFi של
      הבית — והוא יצטרף בעצמו.</p>
    <button class="ghost" onclick="scanSetupAps()" style="margin-top:10px; width:100%">חפש רמקולים חדשים</button>
    <div id="setup-aps" style="margin-top:10px"></div>
    <div class="row" style="margin-top:10px">
      <input id="ap-ssid" placeholder="רשת ההגדרה (Nexus-Setup)">
      <input id="ap-name" placeholder="שם לרמקול (סלון)">
    </div>
    <div class="row" style="margin-top:8px">
      <input id="ap-wifi-ssid" placeholder="רשת ה-WiFi שאליה יתחבר">
      <input id="ap-wifi-psk" placeholder="סיסמת ה-WiFi" type="password">
    </div>
    <button onclick="onboardAp()" style="margin-top:10px; width:100%">חבר לרשת</button>
    <div class="status" id="ap-status"></div>
  </div>


  <div class="card" id="control" style="display:none">
    <h2>שליטה <span class="h2sub" id="sel-name"></span></h2>
    <p class="hint">הנגן, העוצמה והצליל של הרמקול שבחרת.</p>

    <div class="transport">
      <button onclick="transport('play')">נגן</button>
      <button class="ghost" onclick="transport('pause')">השהה</button>
      <button class="ghost" onclick="transport('stop')">עצור</button>
    </div>

    <div class="field">
      <div class="lbl"><span>עוצמה</span><span class="val" id="vol-val">–</span></div>
      <input type="range" id="vol" min="0" max="100" oninput="volPreview(this.value)"
             onchange="setVolume(this.value)">
    </div>

    <div class="row" style="margin-top:12px">
      <button class="ghost" onclick="toggleMute()" id="mute-btn">השתק</button>
      <select id="eq" onchange="setEq(this.value)">
        <option value="default">EQ: רגיל</option>
        <option value="bass_boost">EQ: בס</option>
        <option value="vocal">EQ: קול</option>
        <option value="flat">EQ: שטוח</option>
      </select>
    </div>

    <div class="field">
      <div class="lbl"><span>השהיה (ms) — סנכרון חדר</span></div>
      <input type="number" id="delay" min="0" step="5" onchange="setDelay(this.value)">
    </div>

    <!-- Installer trim: level-matches this speaker against the others once, and holds at every
         volume setting. Deliberately separate from the volume slider above. -->
    <div class="field">
      <div class="lbl"><span>כוונון עוצמה (trim)</span><span class="val" id="gain-val">0.0 dB</span></div>
      <input type="range" id="gain" min="-20" max="20" step="0.5" value="0"
             oninput="gainPreview(this.value)" onchange="setGain(this.value)">
    </div>

    <div class="row" style="margin-top:12px">
      <button class="ghost" id="phase-btn" onclick="togglePhase()">היפוך פאזה: כבוי</button>
    </div>

    <button class="ghost" onclick="refresh()" style="margin-top:12px; width:100%">רענן סטטוס</button>
    <div class="status" id="status"></div>
  </div>

  <!-- שליטה מלאה: פעולות שהרמקול מבצע דרך ה-API שלו. הן לא עוברות בערוץ הפקודות החתום,
       שמאשר אותן אבל לא מבצע אותן בפועל (deferred) — אלא ישירות ל-API של המכשיר. -->

  <!-- Installation and tuning: set once, then left alone. Collapsed so the customer sees their
       speakers first instead of four technical panels. -->
  <details class="advanced">
    <summary>הגדרות מתקדמות — התקנה וכיוונון</summary>

    <div class="card">
      <h2>צימוד רמקול חדש</h2>
      <p class="muted">הרמקול צריך להיות במצב setup ולהציג קוד.</p>
      <button class="ghost" onclick="scanSpeakers()" style="margin-top:10px; width:100%">סרוק רמקולים ברשת</button>
      <div id="discovered" style="margin-top:10px"></div>
      <div class="row" style="margin-top:10px">
        <input id="pair-host" placeholder="כתובת IP של הרמקול">
        <input id="pair-name" placeholder="שם (סלון)">
      </div>
      <div class="row" style="margin-top:8px">
        <input id="pair-code" placeholder="קוד setup (6 ספרות)">
        <input id="pair-box" placeholder="box_public_key (מה-mDNS)">
      </div>
      <div class="row" style="margin-top:8px">
        <input id="pair-ssid" placeholder="שם רשת WiFi">
        <input id="pair-psk" placeholder="סיסמת WiFi" type="password">
      </div>
      <button onclick="pairSpeaker()" style="margin-top:10px; width:100%">צמד רמקול</button>
      <div class="status" id="pair-status"></div>
    </div>

    <div class="card" id="device" style="display:none">
      <h2>שליטה מלאה במכשיר</h2>

      <div class="grid2">
        <button class="ghost" onclick="dev('POST','/api/audio/test','בדיקת שמע')">בדיקת שמע</button>
        <button class="ghost" onclick="dev('POST','/api/calibration/start','כיול')">כיול אוטומטי</button>
      </div>

      <div class="field">
        <div class="lbl"><span>עוצמה מדויקת (0-100)</span></div>
        <input type="number" id="vol-num" min="0" max="100" step="1"
               onchange="commit('vol','/api/volume', withSel({volume:parseInt(this.value)}), 'עוצמה')">
      </div>

      <div class="field">
        <div class="lbl"><span>שם הרמקול</span></div>
        <input type="text" id="dev-name" placeholder="לדוגמה: סלון"
               onchange="dev('POST','/api/device/name','שם', {name:this.value})">
      </div>

      <button class="ghost" onclick="devInfo()" style="margin-top:14px; width:100%">מצב חומרה ואבחון</button>
      <pre id="devout"></pre>

      <!-- פעולות הרסניות מופרדות ומאשרות לפני ביצוע. -->
      <div class="danger">
        <button class="ghost" onclick="devConfirm('POST','/api/system/reboot','אתחול הרמקול?')">אתחל</button>
        <button class="ghost" onclick="devConfirm('POST','/api/system/reset-network','לאפס את הגדרות הרשת? הרמקול ינותק.')">אפס רשת</button>
      </div>
    </div>
    <!-- ── מקור + סיגנל: מה נכנס לסטרימר ומה יוצא ממנו ── -->
    <div class="card">
      <h2>מקור ושידור <span class="h2sub" id="sig-state">—</span></h2>

      <div class="field">
        <div class="lbl"><span>מקור אודיו</span></div>
        <select id="src"><option value="">טוען…</option></select>
      </div>

      <div class="field">
        <div class="lbl"><span>יעד השידור</span></div>
        <select id="src-zone"><option value="">טוען…</option></select>
      </div>

      <div class="row" style="margin-top:12px">
        <button onclick="startBroadcast()" id="bc-btn">התחל שידור</button>
        <button class="ghost" onclick="stopBroadcast()">עצור</button>
      </div>

      <!-- The two meters that make the signal path diagnosable: signal at the input but silence at
           the output means the DSP killed it; silence at both means the source is dead. -->
      <div style="margin-top:16px">
        <div class="meter">
          <div class="mrow"><span class="mlabel">כניסה L</span>
            <div class="mbar"><div class="mfill" id="in-l"></div></div>
            <span class="mdb" id="in-l-db">−∞</span></div>
          <div class="mrow"><span class="mlabel">כניסה R</span>
            <div class="mbar"><div class="mfill" id="in-r"></div></div>
            <span class="mdb" id="in-r-db">−∞</span></div>
        </div>
        <div class="meter">
          <div class="mrow"><span class="mlabel">יציאה L</span>
            <div class="mbar"><div class="mfill" id="out-l"></div></div>
            <span class="mdb" id="out-l-db">−∞</span></div>
          <div class="mrow"><span class="mlabel">יציאה R</span>
            <div class="mbar"><div class="mfill" id="out-r"></div></div>
            <span class="mdb" id="out-r-db">−∞</span></div>
        </div>
        <div class="sigstate" id="sig-info">אין שידור</div>
      </div>
    </div>

    <!-- ── DSP מרכזי: משפיע על כל הרמקולים יחד ── -->
    <div class="card">
      <h2>עיבוד מרכזי <span class="h2sub">משפיע על כל הרמקולים</span></h2>

      <div class="field">
        <div class="lbl"><span>עוצמת מאסטר</span><span class="val" id="mv-val">0.0 dB</span></div>
        <input type="range" id="mv" min="-40" max="12" step="0.5" value="0"
               oninput="document.getElementById('mv-val').textContent=(+this.value).toFixed(1)+' dB'"
               onchange="setDsp({master_volume_db:parseFloat(this.value)})">
      </div>

      <div class="field">
        <div class="lbl"><span>הגבר כניסה</span><span class="val" id="ig-val">0.0 dB</span></div>
        <input type="range" id="ig" min="-20" max="20" step="0.5" value="0"
               oninput="document.getElementById('ig-val').textContent=(+this.value).toFixed(1)+' dB'"
               onchange="setDsp({input_gain_db:parseFloat(this.value)})">
      </div>

      <div class="row" style="margin-top:12px">
        <button class="ghost" id="bypass-btn" onclick="toggleBypass()">Bypass: כבוי</button>
        <button class="ghost" onclick="flatEq()">איפוס EQ</button>
      </div>

      <div class="field">
        <div class="lbl"><span>אקולייזר 32 פסים</span><span class="val" id="eq-hint">±10 dB</span></div>
        <div class="eqwrap" id="eqbands"></div>
      </div>
      <div class="status" id="dsp-status"></div>
    </div>

    <!-- ── מדידת מרחקים אקוסטית ── -->
    <div class="card">
      <h2>מדידת מרחקים <span class="h2sub">chirp אקוסטי</span></h2>
      <p class="muted">כל רמקול משמיע צליל סריקה ומודד מה שהמיקרופונים שלו קלטו.</p>

      <div class="row" style="margin-top:12px">
        <div style="flex:1">
          <div class="lbl"><span>RTL חומרה (ms)</span></div>
          <input type="number" id="m-hw" step="0.001" value="0" title="השהיית ההמרה והבאפר">
        </div>
        <div style="flex:1">
          <div class="lbl"><span>RTL רשת (ms)</span></div>
          <input type="number" id="m-net" step="0.1" value="0" title="השהיית השידור">
        </div>
      </div>
      <p class="muted" style="margin-top:6px; font-size:11px">
        שני הערכים נדרשים. שגיאה של 1ms = 34 ס"מ.
      </p>

      <button onclick="measureAll()" id="m-btn" style="margin-top:12px; width:100%">מדוד את כל הרמקולים</button>
      <div id="m-results" style="margin-top:10px"></div>
      <div class="status" id="m-status"></div>
    </div>

    <div class="card">
      <h2>סריקת רשת <span class="h2sub">מתקינים</span></h2>
      <p class="muted">מצא את כל רמקולי Nexus ברשת המקומית.</p>
      <button onclick="scanNetwork()" id="scan-net-btn" style="margin-top:10px">סרוק את הרשת</button>
      <div id="scan-results" style="margin-top:10px"></div>
    </div>
  </details>

</div>

<script>
let sel = null, muted = false, phaseInverted = false, pollTimer = null;
// Controls the user is actively changing. A background poll must not yank a slider out from under
// a drag, so a pending control keeps its local value until its own reply confirms it.
const pending = new Set();
let dspCfg = null;
const EQ_FREQS = [30,40,50,63,80,100,125,160,200,250,315,400,500,630,800,1000,
                  1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500,14000,
                  16000,18000,19000,20000];

// ── Authentication ────────────────────────────────────────────────────────────
// EVERY /api/ route requires the bearer token (GETs included — the speaker list exposes the site's
// topology and /api/pair carries a Wi-Fi PSK). The token arrives one of two ways:
//   • ?t=<token> in the URL — what the control QR encodes, so scanning it lands here authorized;
//   • otherwise from localStorage, saved by a previous visit.
// The token is stripped from the address bar after being stored so it does not sit in browser
// history, bookmarks, or a screenshot of the URL bar.
//
// fetch() itself is wrapped rather than each call site: there are dozens of bare fetch() calls in
// this page, and one missed call is an invisible 401 that looks like a broken feature.
const AUTH_KEY = 'nexus-streamer-token';
(function initToken() {
  const p = new URLSearchParams(location.search);
  const t = p.get('t');
  if (t) {
    try { localStorage.setItem(AUTH_KEY, t); } catch (e) { /* private mode: keep it in memory */ }
    window.__token = t;
    p.delete('t');
    const q = p.toString();
    history.replaceState(null, '', location.pathname + (q ? '?' + q : ''));
  }
})();
function authToken() {
  if (window.__token) return window.__token;
  try { return localStorage.getItem(AUTH_KEY) || ''; } catch (e) { return ''; }
}
const rawFetch = window.fetch.bind(window);
window.fetch = function (input, init) {
  init = init || {};
  const url = (typeof input === 'string') ? input : (input && input.url) || '';
  if (url.startsWith('/api/')) {
    const tok = authToken();
    if (tok) {
      const h = new Headers(init.headers || {});
      if (!h.has('Authorization')) h.set('Authorization', 'Bearer ' + tok);
      init = Object.assign({}, init, {headers: h});
    }
  }
  return rawFetch(input, init);
};

async function api(path, body) {
  const r = await fetch(path, {method:'POST', headers:{'Content-Type':'application/json'},
                              body: JSON.stringify(body||{})});
  return r.json();
}
function withSel(extra){ return Object.assign({speaker: sel}, extra||{}); }
function setStatus(t){ document.getElementById('status').textContent = t; }

// ── סיגנל: מדי כניסה/יציאה ─────────────────────────────────────────────────────
// The bar fraction is computed server-side so the browser and C++ cannot disagree about where the
// bottom of the scale sits.
function paintMeter(prefix, m) {
  const set = (side, frac, db) => {
    const bar = document.getElementById(prefix + '-' + side);
    const lbl = document.getElementById(prefix + '-' + side + '-db');
    bar.style.width = Math.round(frac * 100) + '%';
    lbl.textContent = db <= -60 ? '−∞' : db.toFixed(1);
    lbl.className = 'mdb' + (m.clipping ? ' clip' : '');
  };
  set('l', m.bar_left, m.rms_db_left);
  set('r', m.bar_right, m.rms_db_right);
}

async function pollLevels() {
  try {
    const r = await (await fetch('/api/levels')).json();
    paintMeter('in', r.input);
    paintMeter('out', r.output);
    const st = {playing:'משדר', paused:'מושהה', idle:'לא משדר'}[r.state] || r.state;
    document.getElementById('sig-state').textContent = st;
    const clip = r.output.clipping ? ' · קליפינג' : '';
    document.getElementById('sig-info').textContent =
      r.state === 'playing'
        ? (st + ' · ' + r.targets + ' רמקולים · ' + r.packets_sent.toLocaleString() + ' חבילות' + clip)
        : 'אין שידור';
  } catch (e) { /* a dropped poll is not worth showing; the next one will land */ }
}

// ── מקורות ויעדים ──────────────────────────────────────────────────────────────
async function loadSources() {
  const sb = document.getElementById('src');
  try {
    const r = await (await fetch('/api/sources')).json();
    sb.innerHTML = '';
    for (const d of (r.devices || [])) {
      const o = document.createElement('option');
      o.value = 'device:' + d.id;
      // Loopback devices carry system audio — that is what "play what my Mac is playing" needs.
      o.textContent = (d.loopback ? '' : '') + d.name;
      sb.appendChild(o);
    }
    const f = document.createElement('option');
    f.value = 'file'; f.textContent = 'קובץ אודיו (נתיב)';
    sb.appendChild(f);
    if (!sb.options.length) sb.innerHTML = '<option value="">לא נמצאו מקורות</option>';
  } catch (e) { sb.innerHTML = '<option value="">שגיאה בטעינת מקורות</option>'; }
}

async function loadZones() {
  const zb = document.getElementById('src-zone');
  try {
    const r = await (await fetch('/api/zones')).json();
    zb.innerHTML = '';
    for (const z of (r.zones || [])) {
      const o = document.createElement('option');
      o.value = z.zone_id;
      o.textContent = z.name + ' (' + (z.members || []).length + ' רמקולים)';
      zb.appendChild(o);
    }
    if (!zb.options.length) zb.innerHTML = '<option value="">אין אזורים — צור אזור קודם</option>';
  } catch (e) { zb.innerHTML = '<option value="">שגיאה</option>'; }
}

async function startBroadcast() {
  const src = document.getElementById('src').value;
  const zone = document.getElementById('src-zone').value;
  const info = document.getElementById('sig-info');
  if (!zone) { info.textContent = 'בחר יעד שידור'; return; }
  if (!src) { info.textContent = 'בחר מקור אודיו'; return; }

  let body = {zone_id: zone};
  if (src.startsWith('device:')) { body.kind = 'device'; body.uri = src.slice(7); }
  else {
    const path = prompt('נתיב לקובץ אודיו:');
    if (!path) return;
    body.kind = 'file'; body.uri = path;
  }
  const btn = document.getElementById('bc-btn');
  btn.disabled = true; info.textContent = 'מתחיל…';
  const r = await api('/api/zones/play', body);
  btn.disabled = false;
  if (!r.ok) info.textContent = 'שגיאה: ' + (r.message || 'לא ניתן להתחיל שידור');
}

async function stopBroadcast() {
  // Stopping is per-speaker over the signed channel; stop every member of the selected zone.
  const zone = document.getElementById('src-zone').value;
  if (!zone) return;
  const r = await (await fetch('/api/zones')).json();
  const z = (r.zones || []).find(x => x.zone_id === zone);
  for (const id of ((z && z.members) || [])) await api('/api/transport', {speaker:id, action:'stop'});
  document.getElementById('sig-info').textContent = 'נעצר';
}

// ── DSP מרכזי ──────────────────────────────────────────────────────────────────
function buildEqBands() {
  const wrap = document.getElementById('eqbands');
  wrap.innerHTML = '';
  EQ_FREQS.forEach((f, i) => {
    const d = document.createElement('div');
    d.className = 'eqband';
    const label = f >= 1000 ? (f/1000) + 'k' : f;
    d.innerHTML = `<input type="range" min="-10" max="10" step="0.5" value="0" id="eqb${i}"
                     oninput="eqPreview()" onchange="commitEq()">
                   <span class="f">${label}</span>`;
    wrap.appendChild(d);
  });
}
function eqPreview() {
  const gains = EQ_FREQS.map((_, i) => parseFloat(document.getElementById('eqb'+i).value));
  const max = Math.max(...gains.map(Math.abs));
  document.getElementById('eq-hint').textContent = max > 0 ? ('שיא ' + max.toFixed(1) + ' dB') : '±10 dB';
}
async function commitEq() {
  const gains = EQ_FREQS.map((_, i) => parseFloat(document.getElementById('eqb'+i).value));
  await setDsp({eq_gains_db: gains});
}
async function flatEq() {
  EQ_FREQS.forEach((_, i) => { document.getElementById('eqb'+i).value = 0; });
  eqPreview();
  await setDsp({eq_gains_db: EQ_FREQS.map(() => 0)});
}
async function toggleBypass() {
  await setDsp({bypass: !(dspCfg && dspCfg.bypass)});
}
// The server MERGES a partial body and returns what was actually applied (clamped), so the UI
// renders the engine's truth rather than echoing the request back.
async function setDsp(patch) {
  const st = document.getElementById('dsp-status');
  try {
    const r = await api('/api/dsp', patch);
    applyDsp(r);
    st.textContent = 'עודכן';
  } catch (e) { st.textContent = 'שגיאה בעדכון DSP'; }
}
function applyDsp(c) {
  if (!c || typeof c !== 'object') return;
  dspCfg = c;
  const mv = document.getElementById('mv'), ig = document.getElementById('ig');
  if (typeof c.master_volume_db === 'number') {
    mv.value = c.master_volume_db;
    document.getElementById('mv-val').textContent = c.master_volume_db.toFixed(1) + ' dB';
  }
  if (typeof c.input_gain_db === 'number') {
    ig.value = c.input_gain_db;
    document.getElementById('ig-val').textContent = c.input_gain_db.toFixed(1) + ' dB';
  }
  const b = document.getElementById('bypass-btn');
  b.textContent = 'Bypass: ' + (c.bypass ? 'פעיל' : 'כבוי');
  b.style.borderColor = c.bypass ? 'var(--warn)' : 'var(--border)';
  b.style.color = c.bypass ? 'var(--warn)' : 'var(--text)';
  if (Array.isArray(c.eq_gains_db)) {
    c.eq_gains_db.forEach((g, i) => {
      const el = document.getElementById('eqb'+i);
      if (el) el.value = g;
    });
    eqPreview();
  }
}
async function loadDsp() {
  try { applyDsp(await (await fetch('/api/dsp')).json()); } catch (e) { /* leave defaults */ }
}

// ── מדידת מרחקים ───────────────────────────────────────────────────────────────
// Runs one speaker at a time on purpose: two speakers chirping together would have their arrivals
// overlap in the same recording, and the correlation cannot tell which peak belongs to which.
async function measureAll() {
  const btn = document.getElementById('m-btn');
  const box = document.getElementById('m-results');
  const st = document.getElementById('m-status');
  const hw = parseFloat(document.getElementById('m-hw').value) || 0;
  const net = parseFloat(document.getElementById('m-net').value) || 0;

  const r = await (await fetch('/api/speakers')).json();
  const speakers = (r.speakers || []).filter(s => s.online);
  if (!speakers.length) { st.textContent = 'אין רמקולים מחוברים'; return; }

  btn.disabled = true;
  box.innerHTML = '';
  const rows = [];

  for (const s of speakers) {
    st.textContent = 'מודד ' + s.name + '…';
    let res;
    try {
      res = await api('/api/measure', {speaker: s.device_id, hardware_rtl_ms: hw, network_rtl_ms: net});
    } catch (e) {
      rows.push({name: s.name, error: 'שגיאת תקשורת'});
      continue;
    }
    const d = res.data || {};
    if (!res.ok || !d.ok) {
      rows.push({name: s.name, error: d.message || res.message || 'המדידה נכשלה'});
    } else {
      rows.push({name: s.name, mics: d.mics || [], warn: d.synchronized === false ? d.limitation : ''});
    }
  }

  // Table: one row per speaker-microphone pair, which is the shape the geometry step consumes.
  let html = '<table class="mtab"><tr><th>רמקול</th><th>מיקרופון</th><th>מרחק</th><th>ביטחון</th></tr>';
  let anyWarn = '';
  for (const row of rows) {
    if (row.error) {
      html += `<tr><td>${row.name}</td><td colspan="3" class="bad">${row.error}</td></tr>`;
      continue;
    }
    if (row.warn) anyWarn = row.warn;
    for (const m of row.mics) {
      if (m.valid) {
        // Confidence under 2 means the winning peak barely beat an unrelated one — likely a room
        // reflection rather than the direct arrival.
        const cls = m.confidence >= 2 ? 'good' : 'warn';
        html += `<tr><td>${row.name}</td><td>Mic${m.mic}</td>
                 <td>${m.distance_m.toFixed(2)} m</td>
                 <td class="${cls}">${m.confidence.toFixed(1)}×</td></tr>`;
      } else {
        html += `<tr><td>${row.name}</td><td>Mic${m.mic}</td>
                 <td colspan="2" class="muted-tel">${m.note || 'לא נמדד'}</td></tr>`;
      }
    }
  }
  html += '</table>';
  if (anyWarn) {
    html += `<p class="muted" style="color:var(--warn); margin-top:8px">${anyWarn}</p>`;
  }
  box.innerHTML = html;
  st.textContent = 'הושלם';
  btn.disabled = false;
}

// ── רמקולים ────────────────────────────────────────────────────────────────────
// Playback health per speaker. A speaker that reported NO telemetry says so explicitly — showing
// zeros for it would draw a perfectly healthy stream for a speaker that may not be playing at all.
function telemetryLine(s) {
  if (!s.online) return '';
  const t = s.telemetry;
  if (!t) return '<div class="tel muted-tel">אין נתוני ניגון</div>';

  const bits = [];
  const loss = typeof t.packet_loss_pct === 'number' ? t.packet_loss_pct : null;
  if (loss !== null) {
    // Any loss at all is audible on a steady stream, so it is coloured as soon as it is non-zero.
    const cls = loss >= 1 ? 'bad' : (loss > 0 ? 'warn' : 'good');
    bits.push(`<span class="${cls}">אובדן ${loss.toFixed(2)}%</span>`);
  }
  if (typeof t.buffer_depth === 'number') bits.push('buffer ' + t.buffer_depth);
  if (typeof t.latency_ms === 'number') bits.push('השהיה ' + t.latency_ms.toFixed(1) + 'ms');
  // Underflows and overflow drops are the two that are directly audible as dropouts, so they are
  // shown only when non-zero and always coloured.
  const xruns = (t.underflows || 0) + (t.dropped_overflow || 0);
  if (xruns > 0) bits.push(`<span class="bad">XRUN ${xruns}</span>`);
  if (typeof s.wifi_signal_dbm === 'number') {
    const stale = typeof s.link_age_s === 'number' && s.link_age_s > 30;
    bits.push(`<span class="${stale ? 'muted-tel' : ''}">${s.wifi_signal_dbm}dBm${stale ? ' (ישן)' : ''}</span>`);
  }
  return bits.length ? '<div class="tel">' + bits.join(' · ') + '</div>' : '';
}

async function loadSpeakers() {
  const r = await (await fetch('/api/speakers')).json();
  const box = document.getElementById('speakers');
  if (!r.speakers.length) { box.innerHTML = '<p class="muted">אין רמקולים עדיין — הוסף למטה.</p>'; return; }
  box.innerHTML = '';
  for (const s of r.speakers) {
    const div = document.createElement('div');
    div.className = 'spk' + (s.device_id===sel ? ' sel':'');
    // Show only what the speaker confirmed; an unconfirmed speaker shows a dash, not a guess.
    const vol = (s.confirmed && typeof s.volume === 'number') ? s.volume + '%' : '—';
    const state = s.online ? vol : 'לא מגיב';
    div.innerHTML = `<div style="min-width:0"><div class="name">${s.name}</div>
      <div class="meta">${s.device_id} · ${s.host}${s.mac ? ' · ' + s.mac : ''} · ${state}</div>
      ${telemetryLine(s)}</div>
      <div style="flex:none"><span class="dot ${s.online?'on':'off'}"></span>
      <button class="ghost" style="padding:6px 10px" onclick="event.stopPropagation();removeSpeaker('${s.device_id}')">הסר</button></div>`;
    div.onclick = () => selectSpeaker(s.device_id, s.name);
    box.appendChild(div);
  }
}
async function addSpeaker() {
  const device_id = document.getElementById('add-id').value.trim();
  const name = document.getElementById('add-name').value.trim() || device_id;
  const host = document.getElementById('add-host').value.trim();
  if (!device_id || !host) { alert('צריך מזהה וכתובת IP'); return; }
  await api('/api/speakers', {device_id, name, host});
  document.getElementById('add-id').value = document.getElementById('add-name').value =
    document.getElementById('add-host').value = '';
  loadSpeakers();
}
async function removeSpeaker(id) {
  await api('/api/speakers/remove', {device_id:id});
  if (sel===id){ sel=null; document.getElementById('control').style.display='none'; }
  loadSpeakers();
}
function selectSpeaker(id, name) {
  sel = id;
  document.getElementById('sel-name').textContent = name;
  document.getElementById('control').style.display = 'block';
  document.getElementById('device').style.display = 'block';
  loadSpeakers();
  refresh();
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(refresh, 5000);
}
// Every control renders CONFIRMED state — what the speaker reported — never what the user just
// asked for. A control shows "ממתין" from the moment it is touched until a reply confirms the new
// value; if the speaker reports something else, the control snaps back to the speaker's truth.
// This is why applyConfirmed() is the ONLY function allowed to write into the inputs.
function applyConfirmed(c) {
  if (!c || !c.confirmed) { setStatus('אין עדיין נתונים מהרמקול'); return; }
  if (!pending.has('vol') && typeof c.volume === 'number') {
    document.getElementById('vol').value = c.volume;
    document.getElementById('vol-val').textContent = c.volume;
    const vn = document.getElementById('vol-num'); if (vn) vn.value = c.volume;
  }
  if (!pending.has('mute') && typeof c.muted === 'boolean') {
    muted = c.muted;
    document.getElementById('mute-btn').textContent = muted ? 'בטל השתקה' : 'השתק';
  }
  if (!pending.has('eq') && c.eq_profile) document.getElementById('eq').value = c.eq_profile;
  if (!pending.has('delay') && typeof c.delay_ms === 'number') {
    document.getElementById('delay').value = c.delay_ms;
  }
  if (!pending.has('gain') && typeof c.gain_db === 'number') {
    document.getElementById('gain').value = c.gain_db;
    document.getElementById('gain-val').textContent = c.gain_db.toFixed(1) + ' dB';
  }
  if (!pending.has('phase') && typeof c.phase_invert === 'boolean') {
    phaseInverted = c.phase_invert;
    const pb = document.getElementById('phase-btn');
    pb.textContent = 'היפוך פאזה: ' + (phaseInverted ? 'פעיל' : 'כבוי');
    pb.style.borderColor = phaseInverted ? 'var(--warn)' : 'var(--border)';
    pb.style.color = phaseInverted ? 'var(--warn)' : 'var(--text)';
  }
  const age = typeof c.age_ms === 'number' ? Math.round(c.age_ms/1000) : null;
  setStatus(c.online === false ? 'הרמקול לא מגיב'
            : ('מאושר מהרמקול' + (age !== null ? ' · לפני ' + age + ' שניות' : '')));
}

// Sends a change and settles the control on the speaker's answer. `key` marks the control pending
// so a poll landing mid-flight cannot overwrite what the user is currently dragging.
async function commit(key, path, body, label) {
  pending.add(key);
  try {
    const r = await api(path, body);
    if (!r.ok) { setStatus('שגיאה: ' + (r.message || 'הרמקול דחה את הפקודה')); }
    else setStatus(label + ' — אושר');
    pending.delete(key);
    // The router returns the confirmed snapshot taken AFTER the speaker replied.
    applyConfirmed(r.confirmed);
    if (!r.confirmed) refresh();
  } catch (e) {
    pending.delete(key);
    setStatus('שגיאת תקשורת');
  }
  loadSpeakers();
}

async function refresh() {
  if (!sel) return;
  const r = await api('/api/status', withSel());
  if (!r.ok) { setStatus('הרמקול לא זמין'); loadSpeakers(); return; }
  applyConfirmed(r.confirmed);
  loadSpeakers();
}
function volPreview(v){ document.getElementById('vol-val').textContent = v; pending.add('vol'); }
async function setVolume(v){ await commit('vol','/api/volume', withSel({volume:parseInt(v)}), 'עוצמה'); }
async function toggleMute(){ await commit('mute','/api/mute', withSel({muted:!muted}), 'השתקה'); }
async function setEq(v){ await commit('eq','/api/eq', withSel({eq_profile:v}), 'EQ'); }
async function setDelay(v){ await commit('delay','/api/delay', withSel({delay_ms:parseInt(v)}), 'השהיה'); }
function gainPreview(v){ document.getElementById('gain-val').textContent=(+v).toFixed(1)+' dB'; pending.add('gain'); }
async function setGain(v){ await commit('gain','/api/gain', withSel({gain_db:parseFloat(v)}), 'כוונון'); }
async function togglePhase(){ await commit('phase','/api/phase', withSel({phase_invert:!phaseInverted}), 'פאזה'); }
async function transport(action){ const r=await api('/api/transport', withSel({action}));
  setStatus(r.ok?('פעולה: '+action):'שגיאה'); }

// ── שליטה מלאה: proxy ל-API של הרמקול עצמו ─────────────────────────────────────
// הערוץ החתום מבצע רק volume/mute/delay/EQ; כיול, בדיקת שמע, אתחול ואיפוס מאושרים
// שם אבל לא מבוצעים. לכן הם נשלחים ל-API של המכשיר דרך /api/device.
async function dev(method, path, label, payload) {
  setStatus(label + '…');
  const r = await api('/api/device', withSel({method, path, body: payload || null}));
  if (r.ok) {
    const msg = (r.data && (r.data.message || r.data.status)) || 'בוצע';
    setStatus(label + ': ' + msg);
  } else {
    setStatus(label + ' נכשל: ' + (r.message || (r.data && r.data.error) || 'שגיאה'));
  }
  return r;
}

// Destructive actions ask first — a mis-click that reboots a speaker mid-track is a bad surprise.
async function devConfirm(method, path, question) {
  if (!confirm(question)) return;
  await dev(method, path, 'פעולה');
}

async function devInfo() {
  const out = document.getElementById('devout');
  out.textContent = 'טוען…';
  const [hw, health, net] = await Promise.all([
    api('/api/device', withSel({method:'GET', path:'/api/hardware'})),
    api('/api/device', withSel({method:'GET', path:'/api/health'})),
    api('/api/device', withSel({method:'GET', path:'/api/network'})),
  ]);
  const lines = [];
  if (hw.ok && hw.data) {
    lines.push('מגבר:   ' + (hw.data.amplifier ?? '—'));
    lines.push('אודיו:  ' + (hw.data.audio_streaming ? 'זורם' : 'לא זורם'));
    lines.push('כיול:   ' + (hw.data.calibration ?? '—'));
  }
  if (health.ok && health.data && health.data.checks) {
    lines.push('בריאות: ' + health.data.result);
    for (const [k, v] of Object.entries(health.data.checks)) lines.push('  ' + k + ': ' + v);
  }
  if (net.ok && net.data) {
    lines.push('רשת:    ' + (net.data.mode || '—') + ' ' + (net.data.ip || ''));
    const link = net.data.streamer_link;
    // Show the streamer's signal only while reports are fresh; a stale number is worse than none.
    if (link) {
      lines.push('סיגנל סטרימר: ' + link.wifi_signal_dbm + ' dBm' + (link.fresh ? '' : ' (לא עדכני)'));
    }
  }
  out.textContent = lines.join('\n') || 'אין נתונים';
}

async function scanNetwork(){
  const btn=document.getElementById('scan-net-btn');
  const box=document.getElementById('scan-results');
  btn.disabled=true; btn.textContent='סורק... (עד ~10 שניות)';
  box.innerHTML='';
  try{
    const r=await (await fetch('/api/scan-network')).json();
    const list=r.speakers||[];
    if(!list.length){ box.innerHTML='<p class="muted">לא נמצאו רמקולי Nexus ברשת</p>'; }
    else {
      for(const s of list){
        const div=document.createElement('div'); div.className='spk';
        div.innerHTML=`<div><div class="name">${s.device_id}</div>
          <div class="meta">${s.ip} · ${s.state} · ${s.paired?'משויך':'לא משויך'}</div></div>
          <button style="flex:none;padding:6px 12px" onclick='addFound(${JSON.stringify(JSON.stringify(s))})'>הוסף</button>`;
        box.appendChild(div);
      }
    }
  }catch(e){ box.innerHTML='<p class="muted">סריקה נכשלה</p>'; }
  btn.disabled=false; btn.textContent='סרוק את הרשת';
}
async function addFound(js){
  const s=JSON.parse(js);
  await api('/api/speakers', {device_id:s.device_id, name:s.device_id, host:s.ip});
  loadSpeakers();
}

// A factory-fresh speaker is invisible to both mDNS and the subnet sweep — it has no credentials,
// so it is on no network. It is only visible as the setup AP it broadcasts, which the streamer's
// spare radio can see without associating. The BSSID shown is the AP's MAC: the only hardware
// identifier available before pairing (the device_id arrives with the pairing reply).
async function scanSetupAps(){
  const box=document.getElementById('setup-aps');
  box.innerHTML='<p class="muted">סורק את האוויר…</p>';
  try{
    const r=await (await fetch('/api/scan-setup-aps')).json();
    if(r.error){ box.innerHTML='<p class="muted">'+r.error+'</p>'; return; }
    if(!r.access_points||!r.access_points.length){
      box.innerHTML='<p class="muted">לא נמצאו רמקולים חדשים. ודא שהרמקול דולק ולא חובר עדיין.</p>'; return; }
    box.innerHTML='';
    for(const ap of r.access_points){
      const div=document.createElement('div'); div.className='spk';
      div.innerHTML=`<div><div class="name">${ap.ssid}</div>
        <div class="meta">MAC ${ap.bssid} · עוצמה ${ap.signal}%</div></div>`;
      div.onclick=()=>{ document.getElementById('ap-ssid').value=ap.ssid;
        document.getElementById('ap-status').textContent='נבחר '+ap.ssid+' — הזן את פרטי ה-WiFi'; };
      box.appendChild(div);
    }
  }catch(e){ box.innerHTML='<p class="muted">הסריקה נכשלה</p>'; }
}

// One request drives the whole sequence server-side (join AP → pair → leave). It is deliberately
// not split across calls: the streamer's radio must never stay parked on a speaker's AP because a
// browser tab closed halfway through.
async function onboardAp(){
  const st=document.getElementById('ap-status');
  const ssid=document.getElementById('ap-ssid').value.trim();
  const wifi=document.getElementById('ap-wifi-ssid').value.trim();
  if(!ssid){ st.textContent='בחר רמקול מהרשימה'; return; }
  if(!wifi){ st.textContent='הזן את שם רשת ה-WiFi'; return; }
  st.textContent='מתחבר לרמקול ומוסר את פרטי הרשת… (עד דקה)';
  try{
    const r=await api('/api/onboard-ap',{
      ssid, name:document.getElementById('ap-name').value.trim()||undefined,
      wifi_ssid:wifi, wifi_psk:document.getElementById('ap-wifi-psk').value});
    if(r.ok){
      st.textContent='הצליח — '+(r.device_id||'')+' מצטרף לרשת. הוא יופיע ברשימה בעוד רגע.';
      // The speaker is getting a new address on the real network; give DHCP a moment, then refresh.
      setTimeout(loadSpeakers, 8000);
    } else {
      st.textContent='נכשל: '+(r.message||r.error||'שגיאה לא ידועה');
    }
  }catch(e){ st.textContent='שגיאת רשת מול הסטרימר'; }
}

async function scanSpeakers(){
  const box=document.getElementById('discovered');
  box.innerHTML='<p class="muted">סורק...</p>';
  try{
    const r=await (await fetch('/api/discover')).json();
    if(!r.speakers||!r.speakers.length){ box.innerHTML='<p class="muted">לא נמצאו רמקולים במצב setup</p>'; return; }
    box.innerHTML='';
    for(const s of r.speakers){
      const div=document.createElement('div'); div.className='spk';
      div.innerHTML=`<div><div class="name">${s.device_id}</div>
        <div class="meta">${s.host} · ${s.setup_mode?'setup':'paired'}</div></div>`;
      div.onclick=()=>{ document.getElementById('pair-host').value=s.host;
        document.getElementById('pair-box').value=s.box_public_key;
        document.getElementById('pair-name').value=s.device_id;
        document.getElementById('pair-status').textContent='נבחר '+s.device_id+' — הזן קוד setup ו-WiFi'; };
      box.appendChild(div);
    }
  }catch(e){ box.innerHTML='<p class="muted">סריקה נכשלה</p>'; }
}

async function pairSpeaker(){
  const host=document.getElementById('pair-host').value.trim();
  const body={ host,
    name: document.getElementById('pair-name').value.trim(),
    setup_code: document.getElementById('pair-code').value.trim(),
    box_public_key: document.getElementById('pair-box').value.trim(),
    wifi_ssid: document.getElementById('pair-ssid').value.trim(),
    wifi_psk: document.getElementById('pair-psk').value };
  const ps=document.getElementById('pair-status');
  if(!host||!body.setup_code||!body.box_public_key){ ps.textContent='צריך IP, קוד setup ו-box_public_key'; return; }
  ps.textContent='מצמד...';
  const r=await api('/api/pair', body);
  if(r.ok){ ps.textContent='צומד! '+(r.device_id||''); loadSpeakers();
    ['pair-host','pair-name','pair-code','pair-box','pair-ssid','pair-psk'].forEach(id=>document.getElementById(id).value='');
  } else { ps.textContent='נכשל: '+(r.message||'שגיאה'); }
}

// The QR is fetched (not a bare <img src>) so the wrapped fetch attaches the bearer token; an
// <img> request carries no Authorization header and would 401 into a broken-image icon.
async function loadControlQr() {
  const img = document.getElementById('control-qr');
  const msg = document.getElementById('control-qr-msg');
  if (!img) return;
  try {
    const r = await fetch('/api/control-qr');
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const svg = await r.text();
    img.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg);
    img.style.display = 'inline-block';
    if (msg) msg.style.display = 'none';
  } catch (e) {
    if (msg) msg.textContent = 'לא ניתן לטעון את הקוד — ודא שאתה מחובר.';
  }
}

buildEqBands();
loadSpeakers();
loadSources();
loadZones();
loadDsp();
loadControlQr();
// 10 Hz keeps the bars smooth enough to read a transient without flooding the streamer.
setInterval(pollLevels, 100);
setInterval(loadZones, 10000);
</script>
</body>
</html>)HTML";

// Substitute the "%LOGO_SRC%" placeholder with the embedded Nexus logo data-URI, the same way the
// speaker's Frontend.h does. The big base64 blob stays in LogoAsset.h while the page remains one
// self-contained document with no external asset requests.
inline std::string controlUiHtml() {
  std::string html = kControlUiTemplate;
  const std::string marker = "%LOGO_SRC%";
  const std::string uri = nexus::web::nexusLogoDataUri();
  for (std::size_t pos = html.find(marker); pos != std::string::npos;
       pos = html.find(marker, pos + uri.size())) {
    html.replace(pos, marker.size(), uri);
  }
  return html;
}

}  // namespace nexus::streamer::web
