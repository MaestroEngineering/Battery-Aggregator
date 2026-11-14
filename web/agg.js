// Aggregator page and the MQTT card (System page).
// Uses api(), $, say(), setText(), formMsg(), bindForm() and `timer` from app.js.
'use strict';

const fmt = (v, d, u) => (v === undefined || v === null) ? '-' : Number(v).toFixed(d) + (u ? ' ' + u : '');

function fmtAge(s) {
  if (s < 90) return Math.round(s) + ' s ago';
  if (s < 5400) return Math.round(s / 60) + ' min ago';
  return (s / 3600).toFixed(1) + ' h ago';
}

function renderAgg(a) {
  const st = $('a-state');
  st.textContent = a.state;
  st.className = a.state === 'normal' ? 'ok' : (a.state === 'fail-safe' || a.state === 'refused') ? 'bad' : 'warn';
  setText('a-reason', a.reason || '-');
  const t = a.tx;
  const txs = $('a-tx');
  txs.textContent = !t.enabled ? 'off' : t.sending ? 'on: sending to the inverter' : 'on, but not sending (' + (t.inv_state !== 'transmitting' ? 'port INV ' + t.inv_state : 'output silent') + ')';
  txs.className = t.enabled && t.sending ? 'ok' : 'warn';
  if (document.activeElement !== $('g-tx') && !$('tx-dlg').open) $('g-tx').checked = t.enabled;
  setText('a-tx-port', (t.inv_enabled ? t.inv_state : 'disabled (enable it on the CAN page)') + ', TEC ' + t.tec);
  setText('a-tx-cnt', t.frames + ' / ' + t.failed + (t.blocked_cmds ? ', ' + t.blocked_cmds + ' SPI commands blocked' : ''));
  const o = a.out;
  const banner = $('a-banner');
  if (!a.configured) { banner.textContent = 'Output refused: ' + a.config_problem; banner.classList.remove('hidden'); }
  else banner.classList.add('hidden');
  setText('a-cvl', fmt(o.cvl, 1, 'V') + (Math.abs(o.cvl_raw - o.cvl) > 0.05 && a.state === 'normal' ? ' (sources ' + fmt(o.cvl_raw, 1, 'V') + ', rise held)' : ''));
  const lim = (v, sum, cap, cl) => fmt(v, 1, 'A') + (a.state === 'normal' ? ' (sum ' + fmt(sum, 1) + (cap >= 0 ? ', share cap ' + fmt(cap, 1) : '') + ')' + (cl ? ' clamped' : '') : '');
  setText('a-ccl', lim(o.ccl, o.ccl_sum, o.ccl_share_cap, o.ccl_clamped));
  setText('a-dcl', lim(o.dcl, o.dcl_sum, o.dcl_share_cap, o.dcl_clamped));
  setText('a-dvl', fmt(o.dvl, 1, 'V'));
  setText('a-soc', fmt(o.soc, 1, '%') + ' / ' + fmt(o.soh, 0, '%'));
  setText('a-v', fmt(o.voltage, 2, 'V'));
  setText('a-i', fmt(o.current, 1, 'A') + ' (+ = charging)');
  setText('a-t', fmt(o.temperature, 1, '°C') + ' (max)');
  setText('a-cap', fmt(o.capacity_ah, 0, 'Ah'));
  setText('a-e', o.energy_charged_kwh === undefined ? '-' : fmt(o.energy_charged_kwh, 1) + ' kWh in / ' + fmt(o.energy_discharged_kwh, 1) + ' kWh out');
  setText('a-alarm', o.alarm_bits + (o.charge_enable ? '' : ', charge off') + (o.discharge_enable ? '' : ', discharge off') +
    (o.guard_charge_factor < 1 ? ', guard: charge ×' + o.guard_charge_factor : '') + (o.guard_discharge_factor < 1 ? ', guard: discharge ×' + o.guard_discharge_factor : ''));
  $('a-frames').textContent = a.output_valid
    ? a.frames.map((f) => f.id + '  ' + f.data.replace(/(..)(?!$)/g, '$1 ')).join('\n')
    : '(silent: no frames)';
  // Sources: Blue Nova bank, its pack 1 (bank − pack 2) and pack 2, and the KNY.
  const A = a.sources[0] || {}, Bk = a.sources[1] || {};
  const cols = [{ k: 'bank', s: A }, { k: 'p1', s: A }, { k: 'p2', s: A }, { k: 'kny', s: Bk }];
  const yn = (v) => v ? 'on' : 'OFF';
  const ekwh = (i, o) => (i === undefined) ? '-' : fmt(i, 2) + ' in / ' + fmt(o, 2) + ' out kWh';
  const cell = (s) => s.cell_min_mv === undefined ? '-' : s.cell_min_mv + ' – ' + s.cell_max_mv + ' mV (spread ' + s.cell_spread_mv + ')';
  const rows = [
    ['Enabled', (c) => c.k === 'p1' || c.k === 'p2' ? (c.s.enabled ? (c.s[c.k === 'p1' ? 'pack1' : 'pack2'] ? 'yes' : 'no data') : 'no') : (c.s.enabled ? 'yes' : 'no')],
    ['State', (c) => c.k === 'p1' || c.k === 'p2' ? '' : c.s.state + (c.s.reason ? ': ' + c.s.reason : '')],
    ['Name', (c) => c.k === 'bank' || c.k === 'kny' ? (c.s.name || '-') : ''],
    ['Last frame', (c) => c.k === 'bank' || c.k === 'kny' ? (c.s.last_frame_age_ms < 0 ? '-' : (c.s.last_frame_age_ms / 1000).toFixed(1) + ' s') : ''],
    ['CVL / DVL', (c) => c.k === 'bank' || c.k === 'kny' ? fmt(c.s.cvl, 1) + ' / ' + fmt(c.s.dvl, 1) + ' V' : ''],
    ['CCL / DCL (own)', (c) => {
      const p = c.k === 'p1' ? c.s.pack1 : c.k === 'p2' ? c.s.pack2 : c.s;
      return p ? fmt(p.ccl, 1) + ' / ' + fmt(p.dcl, 1) + ' A' : '-';
    }],
    ['User cap', (c) => c.k === 'p1' ? '' : c.k === 'p2' ? (c.s.pack2 ? fmt(c.s.pack2.cap_user, 0, 'A') : '-') : fmt(c.s.cap_user, 0, 'A')],
    ['Charge / discharge', (c) => c.k === 'p1' || c.k === 'p2' ? '' : yn(c.s.charge_enable) + ' / ' + yn(c.s.discharge_enable)],
    ['SOC / SOH', (c) => {
      if (c.k === 'p1') return c.s.pack1 && c.s.pack1.soc !== undefined ? fmt(c.s.pack1.soc, 0, '%') + ' (derived)' : '-';
      if (c.k === 'p2') return c.s.pack2 ? fmt(c.s.pack2.soc, 0, '%') : '-';
      return fmt(c.s.soc, 1) + ' / ' + fmt(c.s.soh, 0) + ' %';
    }],
    ['Voltage', (c) => c.k === 'p1' ? '' : c.k === 'p2' ? (c.s.pack2 ? fmt(c.s.pack2.voltage, 2, 'V') : '-') : fmt(c.s.voltage, 2, 'V')],
    ['Current', (c) => {
      const p = c.k === 'p1' ? c.s.pack1 : c.k === 'p2' ? c.s.pack2 : c.s;
      return p ? fmt(p.current, c.k === 'bank' || c.k === 'kny' ? 1 : 2, 'A') : '-';
    }],
    ['Temperature', (c) => c.k === 'p1' ? '' : c.k === 'p2' ? (c.s.pack2 ? fmt(c.s.pack2.temperature, 1, '°C') : '-') : fmt(c.s.temperature, 1, '°C')],
    ['Capacity', (c) => c.k === 'p1' || c.k === 'p2' ? '' : fmt(c.s.capacity_ah, 0, 'Ah') + (c.k === 'kny' && !c.s.capacity_reported ? ' (setting)' : '')],
    ['Cells', (c) => c.k === 'p1' ? '' : c.k === 'p2' ? (c.s.pack2 ? c.s.pack2.cell_min_mv + ' – ' + c.s.pack2.cell_max_mv + ' mV' : '-') : cell(c.s)],
    ['Energy (V × I)', (c) => {
      const p = c.k === 'p1' ? c.s.pack1 : c.k === 'p2' ? c.s.pack2 : c.s;
      return p ? ekwh(p.energy_in_kwh, p.energy_out_kwh) : '-';
    }],
    ['Balancing', (c) => c.k === 'p2' ? (c.s.pack2 && c.s.pack2.balancing ? 'yes' : 'no') : ''],
    ['Alarm bits', (c) => c.k === 'bank' || c.k === 'kny' ? c.s.alarm_bits : ''],
    ['Latched trip', (c) => c.k === 'bank' || c.k === 'kny' ? (c.s.latched ? c.s.latch_reason : 'no') : ''],
    ['Frames ok / rejected / malformed', (c) => c.k === 'bank' || c.k === 'kny' ? c.s.frames_ok + ' / ' + c.s.rejected_frames + ' / ' + c.s.malformed_frames : ''],
    ['Implausible values', (c) => c.k === 'bank' || c.k === 'kny' ? String(c.s.plausibility_rejects) : ''],
    ['Late frames', (c) => c.k === 'bank' || c.k === 'kny' ? ((c.s.frames || []).filter((q) => q.age_ms < 0 || q.age_ms > 3000).map((q) => q.id + (q.pack2 ? 'p2' : '') + (q.age_ms < 0 ? ' never' : ' ' + (q.age_ms / 1000).toFixed(1) + ' s')).join(', ') || 'none') : ''],
  ];
  const tb = $('a-src').tBodies[0];
  tb.textContent = '';
  for (const [label, f] of rows) {
    const tr = document.createElement('tr');
    const th = document.createElement('td'); th.textContent = label; tr.appendChild(th);
    for (const c of cols) { const td = document.createElement('td'); td.textContent = f(c); tr.appendChild(td); }
    tb.appendChild(tr);
  }
  // Over-current guard.
  const gb = $('a-guard').tBodies[0];
  gb.textContent = '';
  for (const g of a.guard || []) {
    const tr = document.createElement('tr');
    for (const t of [g.source, fmt(g.current, 1, 'A'), g.charge + ' (' + fmt(g.charge_limit, 1) + ' A)', g.discharge + ' (' + fmt(g.discharge_limit, 1) + ' A)']) {
      const td = document.createElement('td'); td.textContent = t; tr.appendChild(td);
    }
    if (g.charge !== 'ok' || g.discharge !== 'ok') tr.className = 'bad';
    gb.appendChild(tr);
  }
  // Events.
  const eb = $('a-ev').tBodies[0];
  eb.textContent = '';
  for (const e of a.events) {
    const tr = document.createElement('tr');
    const when = e.unix_ms ? new Date(e.unix_ms).toLocaleString() : fmtAge(e.age_s);
    for (const t of [when, e.source || '-', e.type, e.text]) { const td = document.createElement('td'); td.textContent = t; tr.appendChild(td); }
    if (e.type === 'trip' || e.type === 'stale') tr.className = 'bad';
    eb.appendChild(tr);
  }
  setText('a-mqtt', 'MQTT: ' + a.mqtt.state + (a.mqtt.published ? ', ' + a.mqtt.published + ' messages' : ''));
}

async function refreshAgg() {
  try { renderAgg(await api('/api/agg/status')); } catch (e) { /* 401 handled by api() */ }
}

async function loadAgg() {
  try {
    const s = await api('/api/settings');
    const g = s.agg;
    $('g-a').checked = g.src_a;
    $('g-b').checked = g.src_b;
    $('g-kcap').value = g.kny_capacity_ah;
    $('g-fcvl').value = g.failsafe_cvl || '';
    $('g-fdvl').value = g.failsafe_dvl || '';
    $('g-cclmx').value = g.ccl_max || '';
    $('g-dclmx').value = g.dcl_max || '';
    $('g-act').value = g.silent ? 'silent' : 'values';
    $('g-hold').value = g.startup_hold_s;
    $('g-cvlh').value = g.cvl_hold_s;
    $('g-capa').value = g.cap_a;
    $('g-capp2').value = g.cap_a_pack2;
    $('g-capb').value = g.cap_b;
  } catch (e) { return; }
  refreshAgg();
  timer = setInterval(refreshAgg, 2000);
}

bindForm('f-agg', async (f) => {
  const n = (id) => { const v = $(id).value.trim(); return v === '' ? 0 : Number(v); };
  if ($('g-act').value === 'silent' &&
      !confirm('Silent: when a battery is stale, tripped or not yet seen, the gateway sends nothing and the inverter’s own comms-loss handling takes over. Continue?')) return;
  await api('/api/settings', { method: 'POST', body: { agg: {
    src_a: $('g-a').checked, src_b: $('g-b').checked, kny_capacity_ah: n('g-kcap'),
    failsafe_cvl: n('g-fcvl'), failsafe_dvl: n('g-fdvl'), ccl_max: n('g-cclmx'), dcl_max: n('g-dclmx'),
    silent: $('g-act').value === 'silent', startup_hold_s: n('g-hold'), cvl_hold_s: n('g-cvlh'),
    cap_a: n('g-capa'), cap_a_pack2: n('g-capp2'), cap_b: n('g-capb') } } });
  say(formMsg(f), 'Saved and applied.', 'ok');
  refreshAgg();
});

// Inverter transmit: enabling needs the dialog and the typed word; disabling is immediate.
$('tx-confirm').addEventListener('input', () => { $('tx-ok').disabled = $('tx-confirm').value !== 'TRANSMIT'; });
$('g-tx').addEventListener('change', async () => {
  const box = $('g-tx'), m = $('a-tx-msg');
  if (!box.checked) {
    try { await api('/api/agg/transmit', { method: 'POST', body: { enable: false } }); say(m, 'Transmit off.', 'ok'); }
    catch (e) { say(m, e.message, 'bad'); box.checked = true; }
    refreshAgg();
    return;
  }
  box.checked = false;  // stays off unless confirmed
  const dlg = $('tx-dlg');
  $('tx-confirm').value = '';
  $('tx-ok').disabled = true;
  dlg.returnValue = '';
  dlg.showModal();
  dlg.addEventListener('close', async () => {
    if (dlg.returnValue !== 'ok') { say(m, 'Transmit stays off.', 'ok'); return; }
    try {
      await api('/api/agg/transmit', { method: 'POST', body: { enable: true, confirm: $('tx-confirm').value } });
      box.checked = true;
      say(m, 'Transmit on. The start-up hold (zero limits) applies first.', 'ok');
    } catch (e) { say(m, e.message, 'bad'); }
    refreshAgg();
  }, { once: true });
});

$('a-clear').addEventListener('click', async () => {
  const m = $('a-clear-msg');
  const c = prompt('Clearing a latched trip lets the limits return (after the hold time). Only do this once the cause is understood.\n\nType CLEAR to confirm:');
  if (c === null) return;
  try {
    await api('/api/agg/clear-trips', { method: 'POST', body: { confirm: c } });
    say(m, 'Cleared. Limits return after the hold time if every source is valid.', 'ok');
    refreshAgg();
  } catch (e) { say(m, e.message, 'bad'); }
});

// ---- MQTT card (System page) -----------------------------------------------------------
async function loadMqtt() {
  try {
    const s = await api('/api/settings');
    const m = s.mqtt;
    $('mq-en').checked = m.enabled;
    $('mq-host').value = m.host;
    $('mq-port').value = m.port;
    $('mq-user').value = m.user;
    $('mq-base').value = m.base;
    $('mq-int').value = m.interval_s;
    $('mq-disc').checked = m.discovery;
    $('mq-ret').checked = m.retain;
    $('mq-pass').value = '';
    setText('mq-pass-state', m.password_set ? '(set)' : '(not set)');
  } catch (e) { /* ignore */ }
}

bindForm('f-mqtt', async (f) => {
  const pw = $('mq-pass').value;
  if (pw) await api('/api/mqtt/password', { method: 'POST', body: { password: pw } });
  await api('/api/settings', { method: 'POST', body: { mqtt: {
    enabled: $('mq-en').checked, host: $('mq-host').value.trim(), port: Number($('mq-port').value),
    user: $('mq-user').value, base: $('mq-base').value.trim(), interval_s: Number($('mq-int').value),
    discovery: $('mq-disc').checked, retain: $('mq-ret').checked } } });
  say(formMsg(f), 'Saved. The MQTT client reconnects.', 'ok');
  loadMqtt();
});
