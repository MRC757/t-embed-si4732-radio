// ============================================================
// app.js — Main UI controller
//
// Manages two WebSocket connections:
//   /ws/audio  — binary PCM frames -> audioPlayer + ft8Decoder
//   /ws/radio  — binary waterfall + JSON status
//
// All radio commands are sent as JSON over /ws/radio.
// Band selector and FT8 quick-tune buttons are loaded from REST
// API on startup (/api/bands, /api/ft8freqs).
// ============================================================

'use strict';

// ── WebSocket state ────────────────────────────────────────
let wsAudio = null;
let wsRadio = null;
let reconnectTimer = null;
let connected = false;

// ── Receiver state (kept in sync with ESP32) ──────────────
const state = {
  freqKHz:      10390,
  dialKHz:       10390,
  displayFreqHz: 10390000,
  mode:         'FM',
  band:         'FM',
  bandIndex:     0,
  rssi:          0,
  snr:           0,
  stereo:        false,
  volume:        40,
  bfoHz:         0,
  agc:           true,
  ssb:           true,   // software SSB always available
  bat:           0,
  batPct:       -1,
  charging:      false,
  usbIn:         false,
  rdsName:       '',
  rdsProg:       '',
  sampleRate:    16000,
};

let ft8Freqs = [];

// ============================================================
// WebSocket management
// ============================================================
function connectWebSockets() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const host  = location.host;
  setConnectionStatus('connecting');

  // Audio WebSocket
  wsAudio = new WebSocket(`${proto}//${host}/ws/audio`);
  wsAudio.binaryType = 'arraybuffer';
  wsAudio.onmessage = (e) => {
    if (!(e.data instanceof ArrayBuffer)) return;
    if (audioPlayer.isRunning) audioPlayer.pushFrame(e.data);
    if (ft8Decoder.isRunning)  ft8Decoder.pushAudioFrame(e.data);
  };
  wsAudio.onclose = () => scheduleReconnect();

  // Radio WebSocket
  wsRadio = new WebSocket(`${proto}//${host}/ws/radio`);
  wsRadio.binaryType = 'arraybuffer';
  wsRadio.onopen  = () => { setConnectionStatus('connected'); connected = true; };
  wsRadio.onclose = () => { setConnectionStatus('disconnected'); connected = false; scheduleReconnect(); };
  wsRadio.onmessage = (e) => {
    if (e.data instanceof ArrayBuffer) {
      const dv = new DataView(e.data);
      if (dv.getUint32(0, true) === 0x57465246) waterfall.pushRow(e.data);
    } else {
      try {
        const s = JSON.parse(e.data);
        if (s.type === 'status') {
          applyStatus(s);
          if (s.bands) populateBandSelector(s.bands);
        }
      } catch (_) {}
    }
  };
}

function scheduleReconnect() {
  clearTimeout(reconnectTimer);
  setConnectionStatus('disconnected');
  reconnectTimer = setTimeout(connectWebSockets, 3000);
}

function sendCommand(obj) {
  if (wsRadio && wsRadio.readyState === WebSocket.OPEN) {
    wsRadio.send(JSON.stringify(obj));
  }
}

// ============================================================
// Status display
// ============================================================
function applyStatus(s) {
  Object.assign(state, {
    freqKHz:      s.freq      ?? state.freqKHz,
    dialKHz:      s.freq      ?? state.dialKHz,
    displayFreqHz:s.freqHz    ?? state.displayFreqHz,
    mode:         s.mode      ?? state.mode,
    band:         s.band      ?? state.band,
    bandIndex:    s.bandIndex ?? state.bandIndex,
    rssi:         s.rssi      ?? state.rssi,
    snr:          s.snr       ?? state.snr,
    stereo:       s.stereo    ?? state.stereo,
    volume:       s.volume    ?? state.volume,
    bfoHz:        s.bfoHz     ?? state.bfoHz,
    agc:          s.agc       ?? state.agc,
    bat:          s.bat       ?? state.bat,
    batPct:       s.batPct    ?? state.batPct,
    charging:     s.charging  ?? state.charging,
    usbIn:        s.usbIn     ?? state.usbIn,
    rdsName:      s.rdsName   ?? '',
    rdsProg:      s.rdsProg   ?? '',
    sampleRate:   s.sampleRate ?? state.sampleRate,
  });

  // Feed ESP32 UTC timestamp into FT8 decoder for accurate slot alignment.
  // Status frames arrive every ~500ms, continuously refreshing the reference.
  if (s.utcMs) {
    ft8Decoder.setNtpReference(s.utcMs, Date.now());
    updateNtpStatus(s.ntpSynced, s.utcMs);
  }

  updateFrequencyDisplay();
  updateModeButtons();
  updateMeters();
  updateRDS();
  updateBFORow();
  updateVolume();
  updateBattery();
  updateFooter(s);
}

function updateFrequencyDisplay() {
  const f   = state.freqKHz;
  const fHz = state.displayFreqHz || (f * 1000);
  const isSSB = ['LSB','USB','CW'].includes(state.mode);
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
// Reference: S9 = 34 dBμV (HF), each S-unit = 6 dB.
function rssiToSmeter(rssi) {
  const sUnits = Math.max(0, Math.min(9, 9 + Math.floor((rssi - 34) / 6)));
  const above  = rssi - 34;
  return above > 0 ? 'S9+' + above : 'S' + sUnits;
}

function updateMeters() {
  const rPct = Math.min(100, (state.rssi / 80) * 100);
  const sPct = Math.min(100, (state.snr  / 50) * 100);
  document.getElementById('rssi-bar').style.width   = rPct + '%';
  document.getElementById('snr-bar').style.width    = sPct + '%';
  document.getElementById('rssi-value').textContent = rssiToSmeter(state.rssi);
  document.getElementById('snr-value').textContent  = state.snr;
}

function updateRDS() {
  document.getElementById('rds-name').textContent = state.rdsName || '';
  document.getElementById('rds-info').textContent = state.rdsProg || '';
}

function updateBFORow() {
  const ssbModes = ['LSB','USB','CW'];
  const show = ssbModes.includes(state.mode);
  document.getElementById('bfo-row').style.display = show ? 'flex' : 'none';
  if (show && state.bfoHz) {
    document.getElementById('bfo-value').textContent = 'BFO ' + state.bfoHz + ' Hz';
  }
}

function updateVolume() {
  document.getElementById('volume-slider').value      = state.volume;
  document.getElementById('volume-value').textContent = state.volume;
  document.getElementById('agc-toggle').checked       = state.agc;
}

function updateBattery() {
  const el = document.getElementById('bat-display');

  const v   = state.bat || 0;
  const pct = (state.batPct >= 0) ? state.batPct : -1;

  if (pct < 0) {
    el.textContent = '';
    el.title = 'Battery unavailable';
    return;
  }

  // ASCII-safe icons: use text symbols, no surrogate pairs
  const icon = state.charging ? '[+]' : '[B]';
  el.textContent = icon + pct + '%';
  el.title = 'Battery ' + v.toFixed(2) + 'V  ' + pct + '%'
             + (state.charging ? '  Charging' : '');

  if (state.charging)   el.style.color = '#ffe040';
  else if (pct <= 20)   el.style.color = '#ff3d3d';
  else                  el.style.color = '#00e676';
}

function updateFooter(s) {
  document.getElementById('footer-ssb').textContent  = 'SSB: software (no patch)';
  document.getElementById('footer-dropped').textContent = 'Dropped: ' + (s.dropped ?? 0);
  document.getElementById('footer-ts').textContent   = new Date().toLocaleTimeString();
}

function updateNtpStatus(synced, utcMs) {
  const el = document.getElementById('ntp-status');
  if (!el) return;
  if (synced && utcMs) {
    const t = new Date(utcMs);
    const hms = t.toISOString().slice(11, 19); // HH:MM:SS
    el.textContent = 'Synced ' + hms + ' UTC';
    el.className = 'ntp-status ntp-synced';
  } else {
    el.textContent = 'Not synced — connect to internet or sync manually';
    el.className = 'ntp-status ntp-unsynced';
  }
}

// Load current NTP server from device and show sync state
async function loadNtpSettings() {
  try {
    const res = await fetch('/api/ntp');
    const d   = await res.json();
    const inp = document.getElementById('ntp-server');
    if (inp) inp.value = d.server || 'pool.ntp.org';
    updateNtpStatus(d.synced, d.utcMs);
  } catch (_) {}
}

function setConnectionStatus(status) {
  const dot   = document.getElementById('connection-dot');
  const label = document.getElementById('connection-label');
  dot.className = 'dot dot-' + status;
  label.textContent = ({ connected:'Connected', disconnected:'Disconnected',
                         connecting:'Connecting...' })[status] || status;
}

function updateAudioLatency() {
  const el = document.getElementById('audio-latency');
  if (audioPlayer.isRunning) el.textContent = audioPlayer.latencyMs + 'ms';
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
// FT8 quick-tune buttons
// ============================================================
async function loadFT8Freqs() {
  try {
    const res = await fetch('/api/ft8freqs');
    ft8Freqs  = await res.json();
    const container = document.getElementById('ft8-freq-buttons');
    container.innerHTML = '';
    ft8Freqs.forEach(f => {
      const btn = document.createElement('button');
      btn.className   = 'btn btn-secondary btn-small';
      btn.textContent = f.band;
      btn.title       = f.band + ' FT8 - ' + (f.freq / 1000).toFixed(3) + ' MHz USB';
      btn.addEventListener('click', () => {
        sendCommand({ cmd: 'tune', freq: f.freq });
        sendCommand({ cmd: 'mode', mode: 'USB' });
      });
      container.appendChild(btn);
    });
  } catch (_) {}
}

// ============================================================
// FT8 log display
// ============================================================
function appendFT8Messages(messages, slotTime) {
  const log = document.getElementById('ft8-log');
  messages.forEach(m => {
    const row   = document.createElement('div');
    row.className = 'ft8-row';
    const isCQ  = m.msg.toUpperCase().includes('CQ');
    const snr   = (m.snr >= 0 ? '+' : '') + m.snr;
    const dt    = (m.dt >= 0  ? '+' : '') + m.dt.toFixed(1);
    row.innerHTML =
      '<span class="ft8-time">'  + (m.time ?? slotTime) + '</span>' +
      '<span class="ft8-snr">'   + snr  + '</span>' +
      '<span class="ft8-dt">'    + dt   + '</span>' +
      '<span class="ft8-freq">'  + Math.round(m.freq) + '</span>' +
      '<span class="ft8-msg ' + (isCQ ? 'ft8-cq' : '') + '">' + m.msg + '</span>';
    log.insertBefore(row, log.firstChild);
  });
  while (log.children.length > 200) log.removeChild(log.lastChild);
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
  document.getElementById('btn-seek-up').addEventListener('click',   () => sendCommand({ cmd: 'seek_up' }));
  document.getElementById('btn-seek-down').addEventListener('click', () => sendCommand({ cmd: 'seek_down' }));

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

  // BFO trim slider (+-500 Hz around band default)
  document.getElementById('bfo-slider').addEventListener('input', e => {
    const trim = parseInt(e.target.value, 10);
    document.getElementById('bfo-value').textContent = (trim >= 0 ? '+' : '') + trim + ' Hz';
    sendCommand({ cmd: 'bfo', hz: trim });
    audioPlayer.setBrowserBFO(trim);
  });
  document.getElementById('btn-bfo-reset').addEventListener('click', () => {
    document.getElementById('bfo-slider').value = 0;
    document.getElementById('bfo-value').textContent = '+0 Hz';
    sendCommand({ cmd: 'bfo', hz: 0 });
    audioPlayer.setBrowserBFO(0);
  });

  // Audio start/stop
  document.getElementById('btn-audio-play').addEventListener('click', async () => {
    const statusEl = document.getElementById('audio-status');
    try {
      await audioPlayer.start();
      document.getElementById('btn-audio-play').classList.add('hidden');
      document.getElementById('btn-audio-stop').classList.remove('hidden');
      statusEl.textContent = 'Streaming - ' + state.sampleRate + ' Hz PCM';
    } catch (err) {
      statusEl.textContent = 'Audio error: ' + err.message;
    }
  });
  document.getElementById('btn-audio-stop').addEventListener('click', () => {
    audioPlayer.stop();
    document.getElementById('btn-audio-play').classList.remove('hidden');
    document.getElementById('btn-audio-stop').classList.add('hidden');
    document.getElementById('audio-status').textContent = 'Stopped.';
    document.getElementById('audio-latency').textContent = '';
  });

  // Waterfall toggle
  document.getElementById('waterfall-enabled').addEventListener('change', e => {
    waterfall.setEnabled(e.target.checked);
    if (!e.target.checked) waterfall.clear();
  });

  // NTP server save
  document.getElementById('btn-ntp-save').addEventListener('click', async () => {
    const srv = document.getElementById('ntp-server').value.trim();
    const el  = document.getElementById('ntp-status');
    if (!srv) return;
    el.textContent = 'Saving…';
    el.className   = 'ntp-status';
    try {
      const res = await fetch('/api/ntp', {
        method:  'POST',
        headers: { 'Content-Type': 'application/json' },
        body:    JSON.stringify({ server: srv }),
      });
      const d = await res.json();
      el.textContent = d.ok ? 'Saved — waiting for sync…' : ('Error: ' + (d.error || '?'));
      el.className   = d.ok ? 'ntp-status' : 'ntp-status ntp-unsynced';
    } catch (e) {
      el.textContent = 'Error: ' + e.message;
      el.className   = 'ntp-status ntp-unsynced';
    }
  });

  // Decoder mode (FT8 / JS8Call slot duration)
  document.getElementById('decoder-mode').addEventListener('change', e => {
    ft8Decoder.setSlotMode(e.target.value);
  });

  // FT8 manual sync — snap decoder slot boundary to current moment.
  // Useful when NTP is unavailable: press at the instant you hear
  // the first FT8 tones to align the 15-second buffer window.
  document.getElementById('btn-ft8-sync').addEventListener('click', () => {
    if (!ft8Decoder.isRunning) {
      document.getElementById('ft8-status').textContent =
        'Start FT8 first, then press Sync Now when you hear the signal.';
      return;
    }
    ft8Decoder.manualSync();
  });

  // FT8 start
  document.getElementById('btn-ft8-start').addEventListener('click', async () => {
    const statusEl = document.getElementById('ft8-status');

    // Step 1: audio must be running (user gesture required for AudioContext)
    if (!audioPlayer.isRunning) {
      try {
        await audioPlayer.start();
        document.getElementById('btn-audio-play').classList.add('hidden');
        document.getElementById('btn-audio-stop').classList.remove('hidden');
        document.getElementById('audio-status').textContent =
          'Streaming (started by FT8) - ' + state.sampleRate + ' Hz PCM';
      } catch (err) {
        statusEl.textContent =
          'Audio error: ' + err.message +
          ' - click "Start Audio" first, then try FT8 again.';
        return;
      }
    }

    // Step 2: init decoder worker on first use
    if (!ft8Decoder._worker) {
      statusEl.textContent = 'Loading ft8_lib...';
      try {
        await ft8Decoder.init(
          (msgs, slot) => appendFT8Messages(msgs, slot),
          (msg)        => { statusEl.textContent = msg; }
        );
      } catch (err) {
        statusEl.textContent = 'FT8 init failed: ' + err.message;
        return;
      }
    }

    // Step 3: start aligned decode cycle
    ft8Decoder.start(state.dialKHz || state.freqKHz);
    document.getElementById('btn-ft8-start').classList.add('hidden');
    document.getElementById('btn-ft8-stop').classList.remove('hidden');
  });

  // FT8 stop
  document.getElementById('btn-ft8-stop').addEventListener('click', () => {
    ft8Decoder.stop();
    document.getElementById('btn-ft8-start').classList.remove('hidden');
    document.getElementById('btn-ft8-stop').classList.add('hidden');
    document.getElementById('ft8-slot-timer').textContent = '';
  });

  // FT8 log clear
  document.getElementById('btn-ft8-clear').addEventListener('click', () => {
    document.getElementById('ft8-log').innerHTML = '';
  });
}

// ============================================================
// Entry point
// ============================================================
document.addEventListener('DOMContentLoaded', async () => {
  wireControls();
  await loadBandSelectorFallback();
  await loadFT8Freqs();
  await loadNtpSettings();
  connectWebSockets();
  console.log('[App] T-Embed SI4732 Web Radio ready.');
});
