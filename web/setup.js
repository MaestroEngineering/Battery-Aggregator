// First-boot setup page (served without login only while the device is not set up).
'use strict';

const $ = (id) => document.getElementById(id);

function say(text, kind) {
  const m = $('msg');
  m.textContent = text;
  m.className = 'msg ' + (kind || '');
  m.classList.toggle('hidden', !text);
}

async function call(path, body) {
  const init = { method: body ? 'POST' : 'GET', headers: {} };
  if (body) {
    init.headers['X-BMS-Request'] = '1';
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(body);
  }
  const r = await fetch(path, init);
  const d = await r.json();
  if (!r.ok) throw new Error(d.error || r.statusText);
  return d;
}

$('scan').addEventListener('click', async () => {
  const b = $('scan');
  b.disabled = true; b.textContent = 'Scanning…';
  try {
    const list = await call('/api/setup/scan');
    const dl = $('ssid-list');
    dl.textContent = '';
    list.forEach((n) => {
      const o = document.createElement('option');
      o.value = n.ssid;
      o.label = n.rssi + ' dBm' + (n.secure ? '' : ' (open)');
      dl.appendChild(o);
    });
    say(list.length ? list.length + ' networks found; pick one in the WiFi field.' : 'No networks found.', '');
  } catch (e) { say(e.message, 'bad'); }
  b.disabled = false; b.textContent = 'Scan';
});

async function poll() {
  let s;
  try { s = await call('/api/setup/status'); } catch (e) {
    // The phone may briefly drop off the setup AP while the device changes channel.
    setTimeout(poll, 1500);
    return;
  }
  if (s.state === 'connecting') { say(s.detail, ''); setTimeout(poll, 1000); return; }
  if (s.state === 'connected') {
    say('Connected! IP address ' + s.ip + '. Reconnect your phone or PC to your normal WiFi and open http://' +
      s.hostname + '.local/ (or http://' + s.ip + '/). ' + s.detail, 'ok');
    $('go').disabled = true;
    return;
  }
  say('Could not join the WiFi: ' + s.detail + '. Nothing was saved; check the details and try again.', 'bad');
  $('go').disabled = false;
}

$('setup').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  if ($('admin').value !== $('admin2').value) return say('The admin passwords do not match.', 'bad');
  $('go').disabled = true;
  try {
    await call('/api/setup', {
      ssid: $('ssid').value,
      password: $('pass').value,
      admin_password: $('admin').value,
      ap_password: $('appass').value,
    });
    say('Connecting…', '');
    setTimeout(poll, 1000);
  } catch (e) { say(e.message, 'bad'); $('go').disabled = false; }
});
