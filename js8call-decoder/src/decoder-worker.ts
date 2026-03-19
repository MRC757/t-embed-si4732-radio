/**
 * decoder-worker.ts
 *
 * Runs in a dedicated Web Worker.  Owns both the WebSocket connection and the
 * WASM Js8Decoder instance.  The main thread never touches audio data.
 *
 * Main → Worker messages:
 *   { type: 'start',    url: string, mode: number, freqMin: number, freqMax: number }
 *   { type: 'set-mode', mode: number }
 *   { type: 'stop' }
 *
 * Worker → Main messages:
 *   { type: 'ready' }
 *   { type: 'connected' }
 *   { type: 'disconnected' }
 *   { type: 'decode',  freq_hz: number, snr_db: number, message: string, utc: string }
 *   { type: 'error',   message: string }
 */

import init, { Js8Decoder } from '../wasm/pkg/js8call_wasm.js';

// ---- constants ----

const SAMPLE_RATE = 12_000;

// Slot duration in samples for each mode id (0=Slow 1=Normal 2=Fast 3=Turbo).
const SLOT_SAMPLES = [
  30_000 * SAMPLE_RATE / 1_000,   // Slow:  30 s
  15_600 * SAMPLE_RATE / 1_000,   // Normal: 15.6 s  → 187 200 samples
  10_000 * SAMPLE_RATE / 1_000,   // Fast:  10 s
   6_000 * SAMPLE_RATE / 1_000,   // Turbo:  6 s
];

// ---- state ----

let decoder:      Js8Decoder | null = null;
let ws:           WebSocket  | null = null;
let modeId        = 1;
let slotSamples   = SLOT_SAMPLES[modeId];
let samplesInSlot = 0;
let sampleRate    = SAMPLE_RATE; // overridden if server sends metadata
let decimFactor   = 1;           // set if server sends a higher sample rate

// Simple circular accumulator — no SharedArrayBuffer needed.
const BUF_CAP  = SAMPLE_RATE * 60; // 60 s headroom
const buf      = new Float32Array(BUF_CAP);
let   wPtr     = 0;  // write index
let   rPtr     = 0;  // read index
let   bufCount = 0;

// ---- boot ----

async function boot() {
  try {
    await init();
    self.postMessage({ type: 'ready' });
  } catch (e) {
    self.postMessage({ type: 'error', message: String(e) });
  }
}
boot();

// ---- message handler ----

self.addEventListener('message', (e: MessageEvent) => {
  const msg = e.data;
  switch (msg.type) {

    case 'start':
      modeId      = msg.mode ?? 1;
      slotSamples = SLOT_SAMPLES[modeId] ?? SLOT_SAMPLES[1];
      decoder     = new Js8Decoder(modeId, msg.freqMin ?? 200, msg.freqMax ?? 3000);
      samplesInSlot = 0;
      wPtr = rPtr = bufCount = 0;
      openWebSocket(msg.url);
      break;

    case 'set-mode':
      modeId      = msg.mode;
      slotSamples = SLOT_SAMPLES[modeId] ?? SLOT_SAMPLES[1];
      decoder?.set_mode(modeId);
      samplesInSlot = 0;
      break;

    case 'stop':
      ws?.close();
      ws = null;
      decoder?.free();
      decoder = null;
      break;
  }
});

// ---- WebSocket ----

function openWebSocket(url: string) {
  if (ws) { ws.close(); ws = null; }

  ws = new WebSocket(url);
  ws.binaryType = 'arraybuffer';

  ws.addEventListener('open',    ()  => self.postMessage({ type: 'connected' }));
  ws.addEventListener('close',   ()  => self.postMessage({ type: 'disconnected' }));
  ws.addEventListener('error',   (e) => self.postMessage({ type: 'error', message: 'WebSocket error' }));
  ws.addEventListener('message', (e) => handleFrame(e));
}

function handleFrame(e: MessageEvent) {
  // Text frame — JSON metadata from the SDR server.
  if (typeof e.data === 'string') {
    try {
      const meta = JSON.parse(e.data);
      if (typeof meta.sampleRate === 'number' && meta.sampleRate !== SAMPLE_RATE) {
        decimFactor = Math.round(meta.sampleRate / SAMPLE_RATE);
        if (decimFactor < 1) decimFactor = 1;
      }
    } catch { /* ignore malformed text */ }
    return;
  }

  // Binary frame — signed 16-bit little-endian PCM.
  const pcm16 = new Int16Array(e.data as ArrayBuffer);
  const f32   = pcm16ToFloat32(pcm16, decimFactor);

  // Write into circular buffer.
  for (let i = 0; i < f32.length; i++) {
    if (bufCount < BUF_CAP) {
      buf[wPtr] = f32[i];
      wPtr = (wPtr + 1) % BUF_CAP;
      bufCount++;
    }
    // If buffer is truly full, silently drop — prefer audio loss to crash.
  }

  samplesInSlot += f32.length;
  if (samplesInSlot >= slotSamples) {
    samplesInSlot = 0;
    runDecode();
  }
  // NOTE: deliberate — do not stream spectrogram data from this worker.
  // Visualisation is the responsibility of a separate application.
}

// ---- decode ----

function runDecode() {
  if (!decoder || bufCount === 0) return;

  // Drain the circular buffer into a contiguous slice for WASM.
  const chunk = new Float32Array(bufCount);
  for (let i = 0; i < bufCount; i++) {
    chunk[i] = buf[(rPtr + i) % BUF_CAP];
  }
  rPtr    = (rPtr + bufCount) % BUF_CAP;
  bufCount = 0;

  decoder.push_samples(chunk);
  decoder.run_decode();

  const results = decoder.take_results();
  for (let i = 0; i < results.length; i++) {
    const r = results[i] as { freq_hz: number; snr_db: number; message: string };
    self.postMessage({
      type:    'decode',
      freq_hz: r.freq_hz,
      snr_db:  r.snr_db,
      message: r.message,
      utc:     new Date().toISOString(),
    });
  }
}

// ---- PCM conversion ----

/** Convert Int16 PCM to Float32, applying integer decimation if needed. */
function pcm16ToFloat32(pcm16: Int16Array, decim: number): Float32Array {
  const scale = 1 / 32768;
  if (decim <= 1) {
    const out = new Float32Array(pcm16.length);
    for (let i = 0; i < pcm16.length; i++) out[i] = pcm16[i] * scale;
    return out;
  }
  const outLen = Math.floor(pcm16.length / decim);
  const out    = new Float32Array(outLen);
  for (let i = 0; i < outLen; i++) out[i] = pcm16[i * decim] * scale;
  return out;
}
