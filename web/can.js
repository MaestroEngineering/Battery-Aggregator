// CAN tap page, capture recorder and Verbose Diagnostics card.
// Uses api(), $, say(), setText(), formMsg(), bindForm() and `timer` from app.js.
'use strict';

// ---- Binary capture format (see components/webserver/src/can_api.cpp) ---------
// header 24 bytes: "BMSF", u32 count (0xFFFFFFFF = to end), u32 dropped, u32 0, i64 epoch_offset_us
// record 24 bytes: u64 ts_us, u32 id (bit31 ext, bit30 rtr), u8 dlc, u8 port, u8 flags, u8 0, u8[8] data
const REC = 24, HDR = 24;
const ID_EXT = 0x80000000, ID_RTR = 0x40000000, ID_MASK = 0x1FFFFFFF;

function parseChunk(buf) {
  const v = new DataView(buf);
  if (buf.byteLength < HDR || v.getUint32(0, true) !== 0x46534D42) throw new Error('bad capture data');
  let count = v.getUint32(4, true);
  const max = Math.floor((buf.byteLength - HDR) / REC);
  if (count === 0xFFFFFFFF || count > max) count = max;
  return { buf, count, dropped: v.getUint32(8, true), offset: Number(v.getBigInt64(16, true)) };
}

function* records(chunks) {
  for (const c of chunks) {
    const v = new DataView(c.buf);
    for (let i = 0; i < c.count; i++) {
      const o = HDR + i * REC;
      const raw = v.getUint32(o + 8, true);
      const dlc = Math.min(v.getUint8(o + 12), 8);
      const data = new Uint8Array(c.buf, o + 16, dlc);
      const ts = Number(v.getBigUint64(o, true));
      yield { ts, unix: c.offset ? ts + c.offset : 0, id: raw & ID_MASK,
              ext: (raw & ID_EXT) !== 0, rtr: (raw & ID_RTR) !== 0, dlc, data };
    }
  }
}

const hex2 = (b) => b.toString(16).toUpperCase().padStart(2, '0');
const idText = (id, ext) => id.toString(16).toUpperCase().padStart(ext ? 8 : 3, '0');
const dataHex = (d, sep) => Array.from(d, hex2).join(sep);

function format(chunks, fmt) {
  const out = [];
  if (fmt === 'csv') {
    out.push('unix_time,unix_us,ts_us,id,ext,rtr,dlc,data\n');
    for (const r of records(chunks)) {
      const iso = r.unix ? new Date(r.unix / 1000).toISOString() : '';
      out.push(`${iso},${r.unix || ''},${r.ts},0x${idText(r.id, r.ext)},${+r.ext},${+r.rtr},${r.dlc},${dataHex(r.data, '')}\n`);
    }
  } else if (fmt === 'candump') {
    for (const r of records(chunks)) {
      const t = ((r.unix || r.ts) / 1e6).toFixed(6);
      out.push(`(${t}) can0 ${idText(r.id, r.ext)}#${r.rtr ? 'R' : dataHex(r.data, '')}\n`);
    }
  } else {  // Vector ASC, one time base per file
    const all = Array.from(records(chunks));
    const wall = all.some((r) => r.unix);
    const rows = wall ? all.filter((r) => r.unix) : all;
    const t0 = rows.length ? (wall ? rows[0].unix : rows[0].ts) : 0;
    const d = wall && rows.length ? new Date(t0 / 1000) : new Date();
    // Vector style: "Sun Nov 01 10:00:10.000 am 2026" (12-hour clock).
    const p2 = (x) => String(x).padStart(2, '0');
    const h12 = d.getHours() % 12 || 12;
    const stamp = d.toDateString().slice(0, 10) + ' ' + p2(h12) + ':' + p2(d.getMinutes()) + ':' +
      p2(d.getSeconds()) + '.' + String(d.getMilliseconds()).padStart(3, '0') + ' ' +
      (d.getHours() < 12 ? 'am' : 'pm') + ' ' + d.getFullYear();
    out.push(`date ${stamp}\nbase hex  timestamps absolute\ninternal events logged\n// version 9.0.0\n`);
    if (all.length !== rows.length) out.push(`// ${all.length - rows.length} frames without wall-clock time omitted\n`);
    out.push(`Begin Triggerblock ${stamp}\n`);
    for (const r of rows) {
      const t = (((wall ? r.unix : r.ts) - t0) / 1e6).toFixed(6).padStart(11);
      const id = (r.id.toString(16).toUpperCase() + (r.ext ? 'x' : '')).padEnd(15);
      out.push(`${t} 1  ${id} Rx   ${r.rtr ? 'r' : 'd ' + r.dlc + ' ' + dataHex(r.data, ' ')}\n`);
    }
    out.push('End TriggerBlock\n');
  }
  return out;
}

function saveFile(parts, fmt, what) {
  const ext = { csv: 'csv', asc: 'asc', candump: 'log' }[fmt];
  const d = new Date();
  const p = (n) => String(n).padStart(2, '0');
  const name = `bms-agg-${what}-${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}.${ext}`;
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob(parts, { type: 'text/plain' }));
  a.download = name;
  document.body.appendChild(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
  return name;
}

// ---- Capture (WebSocket) ----------------------------------------------------------
const CAP = { ws: null, chunks: [], n: 0, dropped: 0 };
const CAP_MAX = 2000000;

function capRender() {
  setText('cap-n', CAP.n.toLocaleString() + ' frames' + (CAP.ws ? ' (recording)' : ''));
  setText('cap-drop', CAP.dropped ? CAP.dropped + ' (browser or network too slow)' : '0');
  $('cap-start').disabled = !!CAP.ws;
  $('cap-stop').disabled = !CAP.ws;
}

function capStop(msg, kind) {
  if (CAP.ws) { const ws = CAP.ws; CAP.ws = null; ws.close(); }
  capRender();
  if (msg) say($('cap-msg'), msg, kind);
}

$('cap-start').addEventListener('click', () => {
  if (CAP.ws) return;
  const ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws/can');
  ws.binaryType = 'arraybuffer';
  let opened = false;
  let keepalive = null;
  ws.onopen = () => {
    opened = true;
    say($('cap-msg'), 'Recording live frames…', '');
    // Keeps the socket "recently used" so the device's LRU purge never picks it.
    keepalive = setInterval(() => { if (ws.readyState === 1) ws.send('k'); }, 5000);
  };
  ws.onmessage = (ev) => {
    if (!(ev.data instanceof ArrayBuffer)) return;
    try {
      const c = parseChunk(ev.data);
      CAP.chunks.push(c);
      CAP.n += c.count;
      CAP.dropped = c.dropped;
      if (CAP.n >= CAP_MAX) capStop('Stopped at ' + CAP_MAX.toLocaleString() + ' frames: save the capture.', 'warn');
    } catch (e) { /* ignore malformed message */ }
  };
  ws.onclose = () => {
    clearInterval(keepalive);
    if (CAP.ws === ws) capStop(opened ? 'Capture connection closed.' : 'Could not connect (log in again?).', 'bad');
  };
  CAP.ws = ws;
  capRender();
});
$('cap-stop').addEventListener('click', () => capStop('Stopped. ' + CAP.n.toLocaleString() + ' frames captured.', 'ok'));
$('cap-clear').addEventListener('click', () => { CAP.chunks = []; CAP.n = 0; CAP.dropped = 0; capRender(); say($('cap-msg'), '', ''); });
$('cap-save').addEventListener('click', () => {
  if (!CAP.n) return say($('cap-msg'), 'nothing captured yet', 'bad');
  const fmt = $('cap-fmt').value;
  say($('cap-msg'), 'Saved ' + saveFile(format(CAP.chunks, fmt), fmt, 'capture'), 'ok');
});
$('cap-buf').addEventListener('click', async () => {
  try {
    const r = await fetch('/api/can/buffer', { credentials: 'same-origin' });
    if (r.status === 401) return showLogin();
    if (!r.ok) throw new Error(r.statusText);
    const c = parseChunk(await r.arrayBuffer());
    if (!c.count) return say($('cap-msg'), 'the device buffer is empty', 'warn');
    const fmt = $('cap-fmt').value;
    say($('cap-msg'), 'Saved ' + c.count + ' frames to ' + saveFile(format([c], fmt), fmt, 'buffer'), 'ok');
  } catch (e) { say($('cap-msg'), e.message, 'bad'); }
});
setInterval(() => { if (CAP.ws) capRender(); }, 1000);

// ---- Status, settings, detection, live table -----------------------------------------
let prevBytes = {};

function renderTable(rows) {
  rows.sort((a, b) => (a.x - b.x) || (a.id - b.id));
  const tb = $('tbl').tBodies[0];
  const frag = document.createDocumentFragment();
  const next = {};
  for (const r of rows) {
    const key = (r.x ? 'x' : 's') + r.id;
    const prev = prevBytes[key] || '';
    next[key] = r.d;
    const tr = document.createElement('tr');
    if (r.p && r.a > Math.max(3 * r.p, 2000)) tr.className = 'stale';
    const td = (text, cls) => { const c = document.createElement('td'); c.textContent = text; if (cls) c.className = cls; tr.appendChild(c); return c; };
    td(idText(r.id, r.x) + (r.r ? ' R' : ''));
    td(r.n.toLocaleString(), 'num');
    td(r.p ? r.p + ' ms (' + (1000 / r.p).toFixed(r.p > 1000 ? 2 : 1) + ' Hz)' : '-', 'num');
    td(r.a < 1000 ? r.a + ' ms' : (r.a / 1000).toFixed(1) + ' s', 'num');
    td(String(r.l), 'num');
    const dc = td('');
    for (let i = 0; i < r.l; i++) {
      const s = document.createElement('span');
      const byte = r.d.substr(2 * i, 2);
      s.textContent = byte;
      s.className = 'b' + (prev && prev.substr(2 * i, 2) !== byte ? ' chg' : '');
      dc.appendChild(s);
      if (i < r.l - 1) dc.appendChild(document.createTextNode(' '));
    }
    td(r.mn.replace(/(..)(?!$)/g, '$1 '));
    td(r.mx.replace(/(..)(?!$)/g, '$1 '));
    frag.appendChild(tr);
  }
  prevBytes = next;
  tb.textContent = '';
  tb.appendChild(frag);
}

let detectPoll = null;
function renderDetect(d) {
  if (!d.running && !d.results.length) return;
  $('detect-out').classList.remove('hidden');
  const dl = $('detect-list');
  dl.textContent = '';
  for (const r of d.results) {
    const dt = document.createElement('dt'); dt.textContent = (r.bitrate / 1000) + ' kbit/s';
    const dd = document.createElement('dd'); dd.textContent = r.frames + ' frames, ' + r.errors + ' errors';
    if (r.bitrate === d.suggestion) dd.className = 'ok';
    dl.append(dt, dd);
  }
  const dt = document.createElement('dt'); dt.textContent = 'Result';
  const dd = document.createElement('dd'); dd.textContent = d.running ? 'testing…' : d.note;
  dl.append(dt, dd);
  const b = $('detect-apply');
  b.classList.toggle('hidden', d.running || !d.suggestion);
  if (d.suggestion) { b.textContent = 'Use ' + (d.suggestion / 1000) + ' kbit/s'; b.dataset.br = d.suggestion; }
}

async function refreshCan() {
  try {
    const [s, t] = await Promise.all([api('/api/can/status'), api('/api/can/table')]);
    const st = $('c-state');
    st.textContent = s.state + (s.error ? ' (' + s.error + ')' : '');
    st.className = s.state === 'listen-only' ? 'ok' : s.state === 'error' ? 'bad' : '';
    setText('c-bitrate', (s.bitrate / 1000) + ' kbit/s');
    setText('c-frames', s.frames.toLocaleString());
    setText('c-fps', s.fps + ' frames/s');
    setText('c-age', s.last_frame_age_ms < 0 ? 'none yet' : (s.last_frame_age_ms / 1000).toFixed(1) + ' s ago');
    setText('c-ids', s.ids + (s.ids_dropped ? ' (' + s.ids_dropped + ' frames with IDs beyond the table)' : ''));
    setText('c-errors', String(s.bus_errors));
    setText('c-ovr', String(s.rx_overflows));
    setText('c-rec', String(s.rec));
    const bl = $('c-blocked');
    bl.textContent = String(s.blocked_cmds);
    bl.className = s.blocked_cmds ? 'bad' : '';
    setText('c-ring', s.ring_used + ' / ' + s.ring_size + ' frames');
    renderDetect(s.detect);
    renderTable(t);
  } catch (e) { /* 401 handled by api() */ }
}

async function loadCan() {
  try {
    const s = await api('/api/settings');
    $('can-en').checked = s.can.enabled;
    $('can-br').value = String(s.can.bitrate);
  } catch (e) { return; }
  prevBytes = {};
  capRender();
  refreshCan();
  timer = setInterval(refreshCan, 1000);
}

async function saveCan(enabled, bitrate) {
  await api('/api/settings', { method: 'POST', body: { can: { enabled, bitrate: Number(bitrate) } } });
}

bindForm('f-can', async (f) => {
  await saveCan($('can-en').checked, $('can-br').value);
  say(formMsg(f), 'Saved.', 'ok');
  refreshCan();
});

$('can-detect').addEventListener('click', async () => {
  const m = formMsg($('f-can'));
  try {
    await api('/api/can/detect', { method: 'POST' });
    say(m, 'Trying 125, 250 and 500 kbit/s in listen-only mode (about 5 s)…', '');
    if (detectPoll) clearInterval(detectPoll);
    detectPoll = setInterval(async () => {
      const s = await api('/api/can/status');
      renderDetect(s.detect);
      if (!s.detect.running) { clearInterval(detectPoll); detectPoll = null; say(m, s.detect.note, s.detect.suggestion ? 'ok' : 'warn'); }
    }, 1000);
  } catch (e) { say(m, e.message, 'bad'); }
});

$('detect-apply').addEventListener('click', async () => {
  const br = $('detect-apply').dataset.br;
  try {
    await saveCan(true, br);
    $('can-br').value = br;
    $('can-en').checked = true;
    say(formMsg($('f-can')), 'Bitrate set to ' + (br / 1000) + ' kbit/s.', 'ok');
  } catch (e) { say(formMsg($('f-can')), e.message, 'bad'); }
});

$('tbl-reset').addEventListener('click', async () => {
  try { await api('/api/can/table/reset', { method: 'POST' }); prevBytes = {}; refreshCan(); } catch (e) { /* ignore */ }
});

// ---- Verbose Diagnostics (System page) -------------------------------------------------
function kvList(el, pairs) {
  el.textContent = '';
  for (const [k, v, cls] of pairs) {
    const dt = document.createElement('dt'); dt.textContent = k;
    const dd = document.createElement('dd'); dd.textContent = v; if (cls) dd.className = cls;
    el.append(dt, dd);
  }
}

async function refreshDiag() {
  try {
    const s = await api('/api/diag/status');
    const ago = (x) => (x < 0 ? 'never' : x + ' s ago');
    kvList($('dg-status'), [
      ['State', s.state + (s.last_error ? ': ' + s.last_error : ''), s.last_error ? 'bad' : (s.state === 'idle' || s.state === 'uploading') ? 'ok' : ''],
      ['Uploaded', s.batches_ok + ' batches (last ' + ago(s.last_ok_age_s) + ')'],
      ['Failed', String(s.batches_failed) + (s.last_http_status ? ' (last HTTP ' + s.last_http_status + ')' : '')],
      ['Waiting', s.pending_batches + ' batches, ' + s.pending_frames + ' frames in RAM'],
      ['Dropped', String(s.dropped_total), s.dropped_total ? 'warn' : ''],
      ['Events', s.events_total + (s.last_event_age_s >= 0 ? ' (last ' + ago(s.last_event_age_s) + ': ' + s.last_event_id + ' ' + s.last_event_data + ')' : '')],
      ['Trigger', s.trigger_latched ? 'fired (latched until the frame returns to baseline)' : 'armed / idle', s.trigger_latched ? 'warn' : ''],
      ['Batch seq', 'next ' + s.next_seq + ', boot ' + (s.boot_id >>> 0).toString(16)],
    ]);
  } catch (e) { /* 401 handled */ }
}

async function loadDiag() {
  try {
    const s = await api('/api/settings');
    $('dg-en').checked = s.diag.enabled;
    $('dg-url').value = s.diag.url;
    $('dg-tok').value = '';
    $('dg-tok-hint').textContent = (s.diag.token_set ? 'A token is stored. ' : 'No token stored. ') +
      'Leave empty to keep it. Never exported.';
    $('dg-batch').value = s.diag.batch;
    $('dg-flush').value = s.diag.flush_s;
    $('dg-pre').value = s.diag.pretrigger_s;
    $('dg-chg').checked = s.diag.change_only;
    $('tr-en').checked = s.trigger.enabled;
    $('tr-id').value = s.trigger.id;
    $('tr-base').value = s.trigger.baseline;
  } catch (e) { return; }
  refreshDiag();
  timer = setInterval(refreshDiag, 3000);
}

bindForm('f-diag', async (f) => {
  const num = (id, name) => {
    const v = $(id).value.trim();
    if (!/^\d+$/.test(v)) throw new Error(name + ' must be a whole number');
    return Number(v);
  };
  const body = {
    diag: { enabled: $('dg-en').checked, url: $('dg-url').value.trim(), batch: num('dg-batch', 'batch'),
            flush_s: num('dg-flush', 'flush interval'), pretrigger_s: num('dg-pre', 'pre-trigger'),
            change_only: $('dg-chg').checked },
    trigger: { enabled: $('tr-en').checked, id: $('tr-id').value.trim(), baseline: $('tr-base').value.trim() },
  };
  const tok = $('dg-tok').value;
  if (tok) await api('/api/diag/token', { method: 'POST', body: { token: tok } });
  await api('/api/settings', { method: 'POST', body });
  $('dg-tok').value = '';
  say(formMsg(f), 'Saved.', 'ok');
  loadDiagHint();
  refreshDiag();
});
async function loadDiagHint() {
  try {
    const s = await api('/api/settings');
    $('dg-tok-hint').textContent = (s.diag.token_set ? 'A token is stored. ' : 'No token stored. ') +
      'Leave empty to keep it. Never exported.';
  } catch (e) { /* ignore */ }
}

$('dg-test').addEventListener('click', async () => {
  try {
    await api('/api/diag/test-event', { method: 'POST' });
    say(formMsg($('f-diag')), 'Test event raised: the pre-trigger window is uploaded now.', 'ok');
    setTimeout(refreshDiag, 1500);
  } catch (e) { say(formMsg($('f-diag')), e.message, 'bad'); }
});
$('tr-clear').addEventListener('click', async () => {
  try { await api('/api/diag/clear-trigger', { method: 'POST' }); refreshDiag(); } catch (e) { /* ignore */ }
});
