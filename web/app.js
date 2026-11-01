// BMS Aggregator web UI (vanilla JS, no build step, no external resources).
'use strict';

const $ = (id) => document.getElementById(id);
const VIEWS = ['dashboard', 'can', 'system', 'firmware', 'log'];
let timer = null;

// ---- API ------------------------------------------------------------------
class ApiError extends Error {
  constructor(status, msg) { super(msg); this.status = status; }
}

async function api(path, opts = {}) {
  const init = { method: opts.method || 'GET', credentials: 'same-origin', headers: {} };
  if (init.method === 'POST') {
    init.headers['X-BMS-Request'] = '1';  // required by the device on every POST
    if (opts.body !== undefined) {
      init.headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(opts.body);
    }
  }
  const r = await fetch(path, init);
  const type = r.headers.get('Content-Type') || '';
  const data = type.includes('json') ? await r.json() : await r.text();
  if (r.status === 401 && !opts.noAuthRedirect) {
    showLogin();
    throw new ApiError(401, 'login required');
  }
  if (!r.ok) throw new ApiError(r.status, (data && data.error) || r.statusText);
  return data;
}

function say(el, text, kind) {
  if (!el) return;
  el.textContent = text;
  el.className = 'msg ' + (kind || '');
  el.classList.toggle('hidden', !text);
}
const formMsg = (form) => form.querySelector('.msg');

// ---- Formatting -------------------------------------------------------------
function fmtUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600),
    m = Math.floor(s % 3600 / 60), sec = Math.floor(s % 60);
  return (d ? d + 'd ' : '') + (d || h ? h + 'h ' : '') + m + 'm ' + sec + 's';
}
const kb = (b) => (b / 1024).toFixed(1) + ' KB';
function setText(id, v) { const e = $(id); if (e) e.textContent = (v === undefined || v === '') ? '-' : v; }

// ---- Views ------------------------------------------------------------------
function stopTimer() { if (timer) { clearInterval(timer); timer = null; } }

function showLogin() {
  stopTimer();
  VIEWS.forEach((v) => $('v-' + v).classList.add('hidden'));
  $('nav').classList.add('hidden');
  $('logout').classList.add('hidden');
  $('v-login').classList.remove('hidden');
  $('login-pw').focus();
}

function route() {
  let view = location.hash.replace('#', '');
  if (!VIEWS.includes(view)) view = 'dashboard';
  stopTimer();
  $('v-login').classList.add('hidden');
  $('nav').classList.remove('hidden');
  $('logout').classList.remove('hidden');
  VIEWS.forEach((v) => $('v-' + v).classList.toggle('hidden', v !== view));
  document.querySelectorAll('#nav a').forEach((a) =>
    a.classList.toggle('active', a.dataset.view === view));
  ({ dashboard: loadDashboard, can: () => loadCan(), system: loadSystem, firmware: loadFirmware, log: loadLog })[view]();
}

// Dashboard
async function refreshStatus() {
  try {
    const s = await api('/api/status');
    setText('d-version', s.version);
    setText('d-git', 'git: ' + s.git);
    setText('d-build', s.build_date + ' ' + s.build_time);
    setText('d-idf', s.idf);
    setText('d-chip', s.chip + ' rev ' + Math.floor(s.chip_rev / 100) + '.' + (s.chip_rev % 100));
    const st = $('d-selftest');
    st.textContent = s.selftest + (s.selftest_detail ? ' (' + s.selftest_detail + ')' : '');
    st.className = s.selftest === 'passed' ? 'ok' : s.selftest === 'failed' ? 'bad' : 'warn';
    setText('d-uptime', fmtUptime(s.uptime_s));
    setText('d-heap', kb(s.heap_free) + ' (largest block ' + kb(s.heap_largest) + ')');
    setText('d-heapmin', kb(s.heap_min));
    setText('d-reset', s.reset_reason);
    setText('d-time', s.time ? s.time + (s.time_synced ? '' : ' (not synced)') : 'not set (no NTP yet)');
    setText('d-mode', s.wifi_mode);
    setText('d-ssid', s.ssid || 'not connected');
    setText('d-rssi', s.rssi ? s.rssi + ' dBm' : '-');
    setText('d-ip', s.ip);
    setText('d-host', s.hostname + '.local');
    setText('d-ap', s.ap_up ? s.ap_ssid + ' (on)' : 'off');
    setText('d-can', s.can_state === 'off' ? 'off' : s.can_state + ', ' + (s.can_bitrate / 1000) + ' kbit/s, ' + s.can_fps + ' frames/s');
    setText('d-diag', s.diag_state);
  } catch (e) { if (e.status !== 401) setText('d-uptime', 'offline: ' + e.message); }
}
function loadDashboard() { refreshStatus(); timer = setInterval(refreshStatus, 2000); }

// System
async function loadSystem() {
  try {
    const s = await api('/api/settings');
    $('wifi-ssid').value = s.wifi.ssid || '';
    $('wifi-pass').value = '';
    $('host').value = s.system.hostname;
    $('host-preview').textContent = s.system.hostname;
    $('ntp').value = s.system.ntp_server;
    $('tz').value = s.system.timezone;
  } catch (e) { return; /* login shown on 401 */ }
  loadDiag();  // can.js
}

function bindForm(id, fn) {
  const f = $(id);
  f.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const btn = f.querySelector('button[type=submit]');
    btn.disabled = true;
    try { await fn(f); } catch (e) { say(formMsg(f), e.message, 'bad'); } finally { btn.disabled = false; }
  });
}

bindForm('f-wifi', async (f) => {
  await api('/api/wifi', { method: 'POST', body: { ssid: $('wifi-ssid').value, password: $('wifi-pass').value } });
  say(formMsg(f), 'Saved. The device is reconnecting; if it joins another network this page will stop responding.', 'ok');
});

$('wifi-scan').addEventListener('click', async () => {
  const b = $('wifi-scan');
  b.disabled = true; b.textContent = 'Scanning…';
  try {
    const list = await api('/api/wifi/scan');
    const dl = $('wifi-list');
    dl.textContent = '';
    list.forEach((n) => { const o = document.createElement('option'); o.value = n.ssid; o.label = n.rssi + ' dBm'; dl.appendChild(o); });
    say(formMsg($('f-wifi')), list.length + ' networks found; pick one from the SSID field.', '');
  } catch (e) { say(formMsg($('f-wifi')), e.message, 'bad'); }
  b.disabled = false; b.textContent = 'Scan';
});

bindForm('f-password', async (f) => {
  if ($('pw-new').value !== $('pw-new2').value) throw new Error('the new passwords do not match');
  await api('/api/password', { method: 'POST', body: { current: $('pw-cur').value, new: $('pw-new').value } });
  f.reset();
  say(formMsg(f), 'Password changed. Other sessions were logged out.', 'ok');
});

$('host').addEventListener('input', () => { $('host-preview').textContent = $('host').value || '…'; });
bindForm('f-host', async (f) => {
  await api('/api/settings', { method: 'POST', body: { system: { hostname: $('host').value } } });
  say(formMsg(f), 'Saved. Now reachable as ' + $('host').value + '.local', 'ok');
});

bindForm('f-time', async (f) => {
  await api('/api/settings', { method: 'POST', body: { system: { ntp_server: $('ntp').value, timezone: $('tz').value } } });
  say(formMsg(f), 'Saved. Time will resync.', 'ok');
});

$('import-btn').addEventListener('click', async () => {
  const file = $('import-file').files[0];
  if (!file) return say($('import-msg'), 'choose a file first', 'bad');
  try {
    const obj = JSON.parse(await file.text());
    await api('/api/config/import', { method: 'POST', body: obj });
    say($('import-msg'), 'Imported.', 'ok');
    loadSystem();
  } catch (e) { say($('import-msg'), e instanceof SyntaxError ? 'not valid JSON' : e.message, 'bad'); }
});

$('reboot-btn').addEventListener('click', async () => {
  if (!confirm('Restart the device?')) return;
  try {
    const before = await api('/api/health');
    await api('/api/reboot', { method: 'POST' });
    say($('reset-msg'), 'Restarting…', 'warn');
    await waitForReboot(before.boot_id);
    location.reload();
  } catch (e) { say($('reset-msg'), e.message, 'bad'); }
});

$('reset-btn').addEventListener('click', async () => {
  const c = prompt('This erases WiFi, passwords and all settings.\nType RESET to confirm.');
  if (c !== 'RESET') return;
  try {
    await api('/api/factory-reset', { method: 'POST', body: { confirm: 'RESET' } });
    say($('reset-msg'), 'Factory reset done. The device restarts into its setup access point (BMS-Agg-XXXX).', 'warn');
  } catch (e) { say($('reset-msg'), e.message, 'bad'); }
});

// Firmware
function slotText(s) {
  if (!s.has_app) return s.label + ': empty';
  return s.label + ': ' + s.version + ' (built ' + s.build + ', ' + s.state + ')';
}

async function loadFirmware() {
  try {
    const i = await api('/api/ota/info');
    setText('fw-running', slotText(i.running));
    setText('fw-other', slotText(i.other));
    setText('fw-boot', i.boot);
    setText('fw-selftest', i.selftest + (i.pending_verify ? ' (image on probation)' : ''));
    $('rollback-btn').disabled = !i.rollback_possible || i.busy;
    $('rollback-btn').textContent = i.rollback_possible
      ? 'Roll back to ' + i.other.version + ' (' + i.other.label + ')' : 'No previous image to roll back to';
    $('sig-req').checked = i.require_signed_images;
    $('ota-file').dataset.max = i.max_size;
  } catch (e) { /* 401 handled */ }
}

$('sig-req').addEventListener('change', async () => {
  const v = $('sig-req').checked;
  try {
    await api('/api/settings', { method: 'POST', body: { ota: { require_signed_images: v } } });
    say($('ota-msg'), v ? 'Signed images required: uploads will be rejected until signature verification exists.' : 'Unsigned images accepted (developer mode).', v ? 'warn' : '');
  } catch (e) { $('sig-req').checked = !v; say($('ota-msg'), e.message, 'bad'); }
});

function waitForReboot(oldBootId, timeoutMs = 120000) {
  const t0 = Date.now();
  return new Promise((resolve, reject) => {
    const poll = async () => {
      if (Date.now() - t0 > timeoutMs) return reject(new Error('device did not come back within 2 minutes'));
      try {
        const r = await fetch('/api/health', { cache: 'no-store' });
        const h = await r.json();
        if (h.boot_id && h.boot_id !== oldBootId) return resolve();
      } catch (e) { /* still rebooting */ }
      setTimeout(poll, 2000);
    };
    setTimeout(poll, 3000);
  });
}

$('rollback-btn').addEventListener('click', async () => {
  if (!confirm('Boot the firmware in the other slot?')) return;
  try {
    const before = await api('/api/health');
    await api('/api/ota/rollback', { method: 'POST' });
    say($('rollback-msg'), 'Rebooting into the other slot…', 'warn');
    await waitForReboot(before.boot_id);
    say($('rollback-msg'), 'Device is back. Log in again.', 'ok');
    setTimeout(() => location.reload(), 1500);
  } catch (e) { say($('rollback-msg'), e.message, 'bad'); }
});

$('f-ota').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const file = $('ota-file').files[0];
  const msg = $('ota-msg');
  if (!file) return;
  const max = Number($('ota-file').dataset.max || 0);
  if (max && file.size > max) return say(msg, 'file is larger than the OTA slot (' + kb(max) + ')', 'bad');
  let before;
  try { before = await api('/api/health'); } catch (e) { return say(msg, e.message, 'bad'); }

  const btn = $('ota-btn');
  const prog = $('ota-prog');
  const bar = prog.firstElementChild;
  btn.disabled = true;
  prog.classList.remove('hidden');
  bar.style.width = '0%';
  say(msg, 'Uploading ' + file.name + '…', '');

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota/upload');
  xhr.setRequestHeader('X-BMS-Request', '1');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.upload.onprogress = (e) => {
    if (!e.lengthComputable) return;
    const pct = Math.round(e.loaded * 100 / e.total);
    bar.style.width = pct + '%';
    say(msg, pct < 100 ? 'Uploading… ' + pct + '%' : 'Verifying image…', '');
  };
  xhr.onerror = () => { btn.disabled = false; say(msg, 'upload failed (connection lost)', 'bad'); };
  xhr.onload = async () => {
    let r = {};
    try { r = JSON.parse(xhr.responseText); } catch (e) { /* not JSON */ }
    if (xhr.status === 401) { btn.disabled = false; return showLogin(); }
    if (xhr.status !== 200 || !r.ok) {
      btn.disabled = false;
      bar.style.width = '0%';
      return say(msg, 'Rejected: ' + (r.error || xhr.statusText) + '. The running firmware was not changed.', 'bad');
    }
    say(msg, 'Installed ' + r.version + '. Rebooting; the new firmware must pass its self-test…', 'warn');
    try {
      await waitForReboot(before.boot_id);
      say(msg, 'Device is back. Log in again and check the version on the dashboard.', 'ok');
      setTimeout(() => location.reload(), 2000);
    } catch (e) { say(msg, e.message, 'bad'); btn.disabled = false; }
  };
  xhr.send(file);
});

// Log
async function refreshLog() {
  try {
    const pre = $('log');
    const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 20;
    pre.textContent = await api('/api/log');
    if (atBottom) pre.scrollTop = pre.scrollHeight;
  } catch (e) { /* 401 handled */ }
}
function loadLog() {
  refreshLog().then(() => { $('log').scrollTop = $('log').scrollHeight; });
  timer = setInterval(() => { if ($('log-auto').checked) refreshLog(); }, 3000);
}
$('log-refresh').addEventListener('click', refreshLog);

// ---- Login / start-up -----------------------------------------------------------
$('login-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const btn = ev.target.querySelector('button');
  btn.disabled = true;
  say($('login-msg'), '', 'bad');
  try {
    await api('/api/login', { method: 'POST', body: { password: $('login-pw').value }, noAuthRedirect: true });
    $('login-pw').value = '';
    route();
  } catch (e) { say($('login-msg'), e.message, 'bad'); }
  btn.disabled = false;
});

$('logout').addEventListener('click', async () => {
  try { await api('/api/logout', { method: 'POST' }); } catch (e) { /* ignore */ }
  showLogin();
});

window.addEventListener('hashchange', () => { if ($('v-login').classList.contains('hidden')) route(); });

(async () => {
  try {
    const s = await api('/api/session', { noAuthRedirect: true });
    if (s.setup) { location.href = '/'; return; }  // the device serves the setup page
    if (s.authenticated) route(); else showLogin();
  } catch (e) { showLogin(); }
})();
