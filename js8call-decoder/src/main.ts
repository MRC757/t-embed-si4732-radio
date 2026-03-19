/**
 * main.ts — connects the Js8CallDecoder to a minimal control UI.
 *
 * The UI has one job: take a WebSocket URL and display decoded messages.
 * No audio visualisation — that lives in a separate application.
 */

import { Js8CallDecoder, SpeedMode, DecodeResult } from './decoder.js';

// ---- DOM ----

const $url    = document.getElementById('ws-url')      as HTMLInputElement;
const $mode   = document.getElementById('mode-select') as HTMLSelectElement;
const $fMin   = document.getElementById('freq-min')    as HTMLInputElement;
const $fMax   = document.getElementById('freq-max')    as HTMLInputElement;
const $conn   = document.getElementById('btn-connect') as HTMLButtonElement;
const $status = document.getElementById('status')      as HTMLSpanElement;
const $log    = document.getElementById('decode-log')  as HTMLTableSectionElement;
const $clear  = document.getElementById('btn-clear')   as HTMLButtonElement;
const $count  = document.getElementById('decode-count') as HTMLElement;

// ---- state ----

let decoder: Js8CallDecoder | null = null;
let running  = false;
let decodeCount = 0;

// ---- connect / stop ----

$conn.addEventListener('click', async () => {
  if (running) {
    decoder?.stop();
    decoder  = null;
    running  = false;
    $conn.textContent = 'Connect';
    setStatus('idle', 'Stopped');
    return;
  }

  const url  = $url.value.trim();
  const mode = $mode.value as SpeedMode;
  const fMin = parseFloat($fMin.value) || 200;
  const fMax = parseFloat($fMax.value) || 3000;

  if (!url) { setStatus('err', 'Enter a WebSocket URL'); return; }

  decoder = new Js8CallDecoder({ wsUrl: url, mode, freqMin: fMin, freqMax: fMax });

  decoder
    .on('connected',    () => setStatus('ok',   'Connected'))
    .on('disconnected', () => setStatus('warn', 'Disconnected — retrying…'))
    .on('error',        (m) => setStatus('err', m))
    .on('decode',       (r) => appendRow(r));

  try {
    setStatus('busy', 'Connecting…');
    await decoder.start();
    running = true;
    $conn.textContent = 'Stop';
  } catch (err) {
    setStatus('err', String(err));
    decoder = null;
  }
});

// ---- mode switch ----

$mode.addEventListener('change', () => decoder?.setMode($mode.value as SpeedMode));

// ---- clear ----

$clear.addEventListener('click', () => { $log.innerHTML = ''; decodeCount = 0; $count.textContent = 'No decodes yet'; });

// ---- helpers ----

function setStatus(level: 'idle' | 'ok' | 'warn' | 'err' | 'busy', text: string) {
  $status.textContent   = text;
  $status.dataset.level = level;
}

function appendRow(r: DecodeResult) {
  decodeCount++;
  $count.textContent = `${decodeCount} decode${decodeCount === 1 ? '' : 's'}`;
  const tr = document.createElement('tr');
  tr.innerHTML =
    `<td>${r.utc.slice(11, 19)}</td>` +
    `<td>${r.freq_hz.toFixed(0)}</td>` +
    `<td>${r.snr_db.toFixed(1)}</td>` +
    `<td class="msg">${esc(r.message)}</td>`;
  $log.insertBefore(tr, $log.firstChild);
  while ($log.rows.length > 500) $log.deleteRow($log.rows.length - 1);
}

function esc(s: string) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}
