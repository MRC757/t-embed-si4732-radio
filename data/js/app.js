// ============================================================
// app.js — T-Embed SI4732 Web Radio UI controller
//
// Single WebSocket /ws/radio carries:
//   binary  — waterfall rows (ignored, no audio source)
//   JSON    — status updates at ~2 Hz
//
// All radio commands are sent as JSON over /ws/radio.
// Band list is loaded from /api/bands on startup.
// ============================================================

'use strict';

// ── WebSocket state ─────────────────────────────────────────
let wsRadio = null;
let reconnectTimer = null;
let connected = false;

// ── Receiver state (kept in sync with ESP32) ────────────────
const state = {
  freqKHz:       9730,
  dialKHz:       9730,
  displayFreqHz: 97300000,
  mode:          'FM',
  band:          'FM',
  bandIndex:     0,
  rssi:          0,
  rssiPeak:      0,
  snr:           0,
  stereo:        false,
  volume:        40,
  bfoHz:         0,
  agc:           true,
  bat:           0,
  batPct:       -1,
  charging:      false,
  rdsName:       '',
  rdsProg:       '',
};

let seekPending = false;

// ============================================================
// WebSocket management
// ============================================================
function connectWebSockets() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const host  = location.host;
  setConnectionStatus('connecting');

  wsRadio = new WebSocket(`${proto}//${host}/ws/radio`);
  wsRadio.binaryType = 'arraybuffer';
  wsRadio.onopen  = () => { setConnectionStatus('connected'); connected = true; };
  wsRadio.onclose = () => { setConnectionStatus('disconnected'); connected = false; scheduleReconnect(); };
  wsRadio.onmessage = (e) => {
    if (e.data instanceof ArrayBuffer) return; // waterfall binary — ignored (no audio source)
    try {
      const s = JSON.parse(e.data);
      if (s.type === 'status') {
        applyStatus(s);
        if (s.bands) populateBandSelector(s.bands);
      } else if (s.type === 'mem_list') {
        renderMemorySlots(s.slots || []);
      }
    } catch (_) {}
  };
}

function scheduleReconnect() {
  clearTimeout(reconnectTimer);
  reconnectTimer = setTimeout(connectWebSockets, 3000);
}

function sendCommand(obj) {
  if (wsRadio && wsRadio.readyState === WebSocket.OPEN) {
    wsRadio.send(JSON.stringify(obj));
  }
}

// ============================================================
// Status handling
// ============================================================
function applyStatus(s) {
  // Clear seek indicator on next status update (seek is a blocking I2C call;
  // the next broadcast already contains the result)
  if (seekPending) {
    seekPending = false;
    document.getElementById('seek-indicator').classList.add('hidden');
  }

  Object.assign(state, {
    freqKHz:       s.freq      ?? state.freqKHz,
    dialKHz:       s.freq      ?? state.dialKHz,
    displayFreqHz: s.freqHz    ?? state.displayFreqHz,
    mode:          s.mode      ?? state.mode,
    band:          s.band      ?? state.band,
    bandIndex:     s.bandIndex ?? state.bandIndex,
    rssi:          s.rssi      ?? state.rssi,
    rssiPeak:      s.rssiPeak  ?? state.rssiPeak,
    snr:           s.snr       ?? state.snr,
    stereo:        s.stereo    ?? state.stereo,
    volume:        s.volume    ?? state.volume,
    bfoHz:         s.bfoHz     ?? state.bfoHz,
    agc:           s.agc       ?? state.agc,
    bat:           s.bat       ?? state.bat,
    batPct:        s.batPct    ?? state.batPct,
    charging:      s.charging  ?? state.charging,
    rdsName:       s.rdsName   ?? '',
    rdsProg:       s.rdsProg   ?? '',
  });

  updateFrequencyDisplay();
  updateModeButtons();
  updateMeters();
  updateRDS();
  updateBFORow();
  updateVolume();
  updateBattery();
  updateFooter();
}

// ============================================================
// Display update helpers
// ============================================================
function updateFrequencyDisplay() {
  const f   = state.freqKHz;
  const fHz = state.displayFreqHz || (f * 1000);
  const isSSB = ['LSB', 'USB', 'CW'].includes(state.mode);
  let display, unit;

  if (state.mode === 'FM') {
    display = (f / 100).toFixed(1);
    unit = 'MHz';
  } else if (isSSB) {
    const mhz = fHz / 1000000;
    let s = mhz.toFixed(6);
    while (s.endsWith('0') && s.split('.')[1].length > 3) s = s.slice(0, -1);
    display = s;
    unit = 'MHz';
  } else if (f >= 1000) {
    display = (f / 1000).toFixed(3);
    unit = 'MHz';
  } else {
    display = String(f);
    unit = 'kHz';
  }

  document.getElementById('freq-value').textContent = display;
  document.getElementById('freq-unit').textContent  = unit;
  document.getElementById('mode-badge').textContent = state.mode;
  document.getElementById('band-label').textContent = state.band;

  const sb = document.getElementById('stereo-badge');
  if (state.mode === 'FM' && state.stereo) sb.classList.remove('hidden');
  else sb.classList.add('hidden');

  document.getElementById('freq-input').value = f;

  const sel = document.getElementById('band-select');
  if (sel && state.bandIndex != null) sel.value = state.bandIndex;
}

function updateModeButtons() {
  document.querySelectorAll('.btn-mode').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.mode === state.mode);
  });
}

// Convert RSSI (dBμV) to S-meter label.
// S9 = 34 dBμV (HF), each S-unit = 6 dB.
function rssiToSmeter(rssi) {
  const sUnits = Math.max(0, Math.min(9, 9 + Math.floor((rssi - 34) / 6)));
  const above  = rssi - 34;
  return above > 0 ? 'S9+' + above : 'S' + sUnits;
}

function updateMeters() {
  const rPct    = Math.min(100, (state.rssi     / 80) * 100);
  const peakPct = Math.min(100, (state.rssiPeak / 80) * 100);
  const sPct    = Math.min(100, (state.snr      / 50) * 100);
  document.getElementById('rssi-bar').style.width    = rPct + '%';
  document.getElementById('rssi-peak').style.left    = peakPct + '%';
  document.getElementById('rssi-peak').style.display = state.rssiPeak > 0 ? 'block' : 'none';
  document.getElementById('snr-bar').style.width     = sPct + '%';
  document.getElementById('rssi-value').textContent  = rssiToSmeter(state.rssi);
  document.getElementById('snr-value').textContent   = state.snr;
}

function updateRDS() {
  document.getElementById('rds-name').textContent = state.rdsName || '';
  document.getElementById('rds-info').textContent = state.rdsProg || '';
}

function updateBFORow() {
  const show = ['LSB', 'USB', 'CW'].includes(state.mode);
  document.getElementById('bfo-row').style.display = show ? 'flex' : 'none';
  if (show) {
    document.getElementById('bfo-value').textContent =
      (state.bfoHz >= 0 ? '+' : '') + state.bfoHz + ' Hz';
  }
}

function updateVolume() {
  document.getElementById('volume-slider').value      = state.volume;
  document.getElementById('volume-value').textContent = state.volume;
  document.getElementById('agc-toggle').checked       = state.agc;
}

function updateBattery() {
  const el  = document.getElementById('bat-display');
  const v   = state.bat || 0;
  const pct = (state.batPct >= 0) ? state.batPct : -1;

  if (pct < 0) { el.textContent = ''; el.title = 'Battery unavailable'; return; }

  el.textContent = (state.charging ? '[+]' : '[B]') + pct + '%';
  el.title = 'Battery ' + v.toFixed(2) + 'V  ' + pct + '%' + (state.charging ? '  Charging' : '');
  el.style.color = state.charging ? '#ffe040' : pct <= 20 ? '#ff3d3d' : '#00e676';
}

function updateFooter() {
  document.getElementById('footer-ts').textContent = new Date().toLocaleTimeString();
}

function setConnectionStatus(status) {
  const dot   = document.getElementById('connection-dot');
  const label = document.getElementById('connection-label');
  dot.className = 'dot dot-' + status;
  label.textContent = (
    { connected: 'Connected', disconnected: 'Disconnected', connecting: 'Connecting...' }
  )[status] || status;
}

// ============================================================
// Band selector
// ============================================================
function populateBandSelector(bands) {
  const sel = document.getElementById('band-select');
  if (sel.options.length === bands.length) return;
  sel.innerHTML = '';
  bands.forEach((b, i) => {
    const opt = document.createElement('option');
    opt.value       = b.index ?? i;
    opt.textContent = b.name + '  (' + b.mode + ')';
    sel.appendChild(opt);
  });
}

async function loadBandSelectorFallback() {
  try {
    const res  = await fetch('/api/bands');
    const data = await res.json();
    if (data.bands) populateBandSelector(data.bands);
  } catch (_) {}
}

// ============================================================
// UI event wiring
// ============================================================
function wireControls() {

  // Tune
  document.getElementById('btn-tune').addEventListener('click', () => {
    const v = parseInt(document.getElementById('freq-input').value, 10);
    if (!isNaN(v) && v > 0) sendCommand({ cmd: 'tune', freq: v });
  });
  document.getElementById('freq-input').addEventListener('keydown', e => {
    if (e.key === 'Enter') document.getElementById('btn-tune').click();
  });

  // Step / seek
  document.getElementById('btn-step-up').addEventListener('click',   () => sendCommand({ cmd: 'step_up' }));
  document.getElementById('btn-step-down').addEventListener('click', () => sendCommand({ cmd: 'step_down' }));
  document.getElementById('btn-seek-up').addEventListener('click', () => {
    seekPending = true;
    document.getElementById('seek-indicator').classList.remove('hidden');
    sendCommand({ cmd: 'seek_up' });
  });
  document.getElementById('btn-seek-down').addEventListener('click', () => {
    seekPending = true;
    document.getElementById('seek-indicator').classList.remove('hidden');
    sendCommand({ cmd: 'seek_down' });
  });

  // Mode buttons
  document.querySelectorAll('.btn-mode').forEach(btn => {
    btn.addEventListener('click', () => sendCommand({ cmd: 'mode', mode: btn.dataset.mode }));
  });

  // Band selector
  document.getElementById('band-select').addEventListener('change', e => {
    sendCommand({ cmd: 'band', index: parseInt(e.target.value, 10) });
  });

  // Volume
  document.getElementById('volume-slider').addEventListener('input', e => {
    const v = parseInt(e.target.value, 10);
    document.getElementById('volume-value').textContent = v;
    sendCommand({ cmd: 'volume', value: v });
  });

  // AGC
  document.getElementById('agc-toggle').addEventListener('change', e => {
    sendCommand({ cmd: 'agc', enable: e.target.checked, gain: 0 });
  });

  // BFO trim
  document.getElementById('bfo-slider').addEventListener('input', e => {
    const trim = parseInt(e.target.value, 10);
    document.getElementById('bfo-value').textContent = (trim >= 0 ? '+' : '') + trim + ' Hz';
    sendCommand({ cmd: 'bfo', hz: trim });
  });
  document.getElementById('btn-bfo-reset').addEventListener('click', () => {
    document.getElementById('bfo-slider').value = 0;
    document.getElementById('bfo-value').textContent = '+0 Hz';
    sendCommand({ cmd: 'bfo', hz: 0 });
  });
}

// ============================================================
// Memory channels
// ============================================================
function renderMemorySlots(slots) {
  const el = document.getElementById('memory-slots');
  el.innerHTML = '';
  if (!slots.length) {
    el.innerHTML = '<span style="font-size:12px;color:var(--text-dim)">No memory channels saved yet.</span>';
    return;
  }
  slots.forEach(m => {
    const btn = document.createElement('button');
    btn.className = 'btn btn-secondary btn-small mem-btn';
    const freqStr = m.freq >= 1000 ? (m.freq / 1000).toFixed(3) + ' MHz' : m.freq + ' kHz';
    btn.textContent = (m.name ? m.name + '  ' : '') + freqStr + '  ' + m.mode;
    btn.title = 'Slot ' + m.slot + ': load ' + freqStr + ' ' + m.mode;
    btn.addEventListener('click', () => sendCommand({ cmd: 'mem_load', slot: m.slot }));
    el.appendChild(btn);
  });
}

function wireMemoryControls() {
  document.getElementById('btn-mem-save').addEventListener('click', () => {
    const slot  = parseInt(document.getElementById('mem-slot-select').value, 10);
    const label = document.getElementById('mem-name').value.trim();
    sendCommand({ cmd: 'mem_save', slot, name: label });
    document.getElementById('mem-name').value = '';
  });
}

// ============================================================
// Entry point
// ============================================================
document.addEventListener('DOMContentLoaded', async () => {
  wireControls();
  wireMemoryControls();
  await loadBandSelectorFallback();
  connectWebSockets();
  console.log('[App] T-Embed SI4732 Web Radio ready.');
});
