// ============================================================
// js8call-worker.js — Module Web Worker for JS8Call decoding
//
// Loaded as a module worker by js8call.js:
//   new Worker('/js/js8call-worker.js', { type: 'module' })
//
// Requires js8call_wasm.js + js8call_wasm_bg.wasm in data/js/.
// Build from js8call-decoder/ directory:
//   npm install
//   npm run wasm:build
//   cp wasm/pkg/js8call_wasm.js wasm/pkg/js8call_wasm_bg.wasm data/js/
//   pio run -t uploadfs
//
// Without WASM files, falls back to RMS energy reporting (signal
// presence only, no callsign decoding).
//
// Message protocol:
//   Incoming:
//     { type:'start',    mode:number, freqMin:number, freqMax:number }
//     { type:'audio',    buffer:ArrayBuffer }  — /ws/audio frame with 4-byte ts header
//     { type:'set-mode', mode:number }
//     { type:'stop' }
//   Outgoing:
//     { type:'ready',   wasm:bool, msg:string }
//     { type:'decoded', messages:[], slotTime:string }
//     { type:'error',   msg:string }
//
// Speed modes (JS8Call standard, id matches Js8Decoder constructor):
//   0 = Slow   — 30    s slot, 320 ms/symbol, 3840-pt FFT
//   1 = Normal — 15.6  s slot, 160 ms/symbol, 1920-pt FFT
//   2 = Fast   — 10    s slot,  80 ms/symbol,  960-pt FFT
//   3 = Turbo  —  6    s slot,  40 ms/symbol,  480-pt FFT
// ============================================================

const SAMPLE_RATE = 12_000;

// Samples to accumulate before triggering one decode pass.
// Must be >= the frame length so the WASM sees a full transmission.
const SLOT_SAMPLES = [
  30    * SAMPLE_RATE,              // Slow   — 360 000 samples
  Math.round(15.6 * SAMPLE_RATE),  // Normal — 187 200 samples
  10    * SAMPLE_RATE,             // Fast   — 120 000 samples
   6    * SAMPLE_RATE,             // Turbo  —  72 000 samples
];

// ── State ───────────────────────────────────────────────────
let Js8Decoder    = null;  // WASM class — set after init()
let decoder       = null;  // active Js8Decoder instance
let wasmReady     = false;
let modeId        = 1;
let slotSamples   = SLOT_SAMPLES[modeId];
let samplesInSlot = 0;
let freqMin       = 200;
let freqMax       = 3000;

// 60-second circular audio accumulation buffer
const BUF_CAP  = SAMPLE_RATE * 60;
const buf      = new Float32Array(BUF_CAP);
let   wPtr     = 0;
let   rPtr     = 0;
let   bufCount = 0;

// ── WASM boot ───────────────────────────────────────────────
// js8call_wasm.js is the wasm-bindgen glue (--target web).
// Its default export is init(), which fetches js8call_wasm_bg.wasm
// relative to this worker's URL (/js/js8call_wasm_bg.wasm).
try {
  const mod = await import('./js8call_wasm.js');
  await mod.default();          // fetch + compile .wasm
  Js8Decoder = mod.Js8Decoder;
  wasmReady  = true;
  postMessage({ type: 'ready', wasm: true,
    msg: '✓ JS8Call WASM ready — Slow / Normal / Fast / Turbo' });
} catch (_) {
  postMessage({ type: 'ready', wasm: false,
    msg: 'js8call_wasm.js not found — build WASM and run uploadfs. Energy fallback active.' });
}

// ── Message handler ─────────────────────────────────────────
onmessage = function(e) {
  const msg = e.data;
  switch (msg.type) {

    case 'start':
      modeId        = msg.mode  ?? 1;
      freqMin       = msg.freqMin ?? 200;
      freqMax       = msg.freqMax ?? 3000;
      slotSamples   = SLOT_SAMPLES[modeId] ?? SLOT_SAMPLES[1];
      samplesInSlot = 0;
      wPtr = rPtr = bufCount = 0;
      if (wasmReady && Js8Decoder) {
        decoder?.free();
        decoder = new Js8Decoder(modeId, freqMin, freqMax);
      }
      break;

    case 'audio':
      handleFrame(msg.buffer);
      break;

    case 'set-mode':
      modeId      = msg.mode;
      slotSamples = SLOT_SAMPLES[modeId] ?? SLOT_SAMPLES[1];
      if (decoder) {
        decoder.set_mode(modeId);
        samplesInSlot = 0;
      }
      break;

    case 'stop':
      decoder?.free();
      decoder       = null;
      samplesInSlot = 0;
      wPtr = rPtr = bufCount = 0;
      break;
  }
};

// ── Audio frame ingestion ────────────────────────────────────
// Frame format: [4 bytes uint32 timestamp LE][Int16 PCM @ 12 kHz]
// The 4-byte header is stripped by reading from byte offset 4.
function handleFrame(arrayBuffer) {
  const pcm16 = new Int16Array(arrayBuffer, 4);
  const f32   = pcm16ToFloat32(pcm16);

  for (let i = 0; i < f32.length; i++) {
    if (bufCount < BUF_CAP) {
      buf[wPtr] = f32[i];
      wPtr = (wPtr + 1) % BUF_CAP;
      bufCount++;
    }
    // Silently drop on full buffer — prefer audio loss to crash.
  }

  samplesInSlot += f32.length;
  if (samplesInSlot >= slotSamples) {
    samplesInSlot = 0;
    runDecode();
  }
}

// ── Decode pass ──────────────────────────────────────────────
function runDecode() {
  const slotTime = new Date().toISOString().slice(11, 19);

  if (!decoder || !wasmReady) {
    energyFallback(slotTime);
    return;
  }

  // Drain circular buffer into a contiguous Float32Array for WASM.
  const count = bufCount;
  const chunk = new Float32Array(count);
  for (let i = 0; i < count; i++) {
    chunk[i] = buf[(rPtr + i) % BUF_CAP];
  }
  rPtr     = (rPtr + count) % BUF_CAP;
  bufCount = 0;

  try {
    decoder.push_samples(chunk);
    decoder.run_decode();

    // take_results() returns a js_sys::Array of plain JS objects:
    //   { freq_hz: number, snr_db: number, message: string }
    const raw      = decoder.take_results();
    const messages = [];
    for (let i = 0; i < raw.length; i++) {
      const r = raw[i];
      messages.push({
        snr:  r.snr_db,
        dt:   0,
        freq: r.freq_hz,
        msg:  r.message,
        time: slotTime,
      });
    }
    postMessage({ type: 'decoded', messages, slotTime });

  } catch (err) {
    postMessage({ type: 'error', msg: err.message });
  }
}

// ── Energy fallback (no WASM) ────────────────────────────────
function energyFallback(slotTime) {
  const count = bufCount;
  let sum = 0;
  for (let i = 0; i < count; i++) {
    const s = buf[(rPtr + i) % BUF_CAP];
    sum += s * s;
  }
  rPtr     = (rPtr + count) % BUF_CAP;
  bufCount = 0;

  const dB = 10 * Math.log10(sum / Math.max(count, 1) + 1e-12);
  if (dB > -60) {
    postMessage({ type: 'decoded', messages: [{
      snr:  Math.round(dB + 60),
      dt:   0,
      freq: 0,
      msg:  '(WASM not loaded — signal energy: ' + dB.toFixed(1) + ' dBFS)',
      time: slotTime,
    }], slotTime });
  } else {
    postMessage({ type: 'decoded', messages: [], slotTime });
  }
}

// ── PCM helper ───────────────────────────────────────────────
function pcm16ToFloat32(pcm16) {
  const out   = new Float32Array(pcm16.length);
  const scale = 1 / 32768;
  for (let i = 0; i < pcm16.length; i++) out[i] = pcm16[i] * scale;
  return out;
}
