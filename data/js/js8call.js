// ============================================================
// js8call.js — Browser-side JS8Call decoder controller
//
// Decoding is delegated to js8call-worker.js (module Web Worker)
// which uses the js8call_wasm.js / js8call_wasm_bg.wasm WASM library
// built from js8call-decoder/wasm/ (Rust + wasm-pack).
//
// Build the WASM once, then serve from LittleFS:
//   cd js8call-decoder
//   npm install && npm run wasm:build
//   cp wasm/pkg/js8call_wasm.js wasm/pkg/js8call_wasm_bg.wasm data/js/
//   pio run -t uploadfs
//
// Architecture:
//   WebSocket audio frames → pushAudioFrame()
//     → forwarded to js8call-worker.js
//     → worker accumulates samples for one slot duration
//     → worker calls Js8Decoder.run_decode() (LDPC + callsign decode)
//     → decoded messages posted back → displayed in UI
//
// Speed modes:
//   0 = Slow   — 30    s slot (79 × 320 ms symbols, 3125 Hz tone spacing)
//   1 = Normal — 15.6  s slot (79 × 160 ms symbols, 6250 Hz tone spacing)
//   2 = Fast   — 10    s slot (79 ×  80 ms symbols, 12500 Hz tone spacing)
//   3 = Turbo  —  6    s slot (79 ×  40 ms symbols, 25000 Hz tone spacing)
// ============================================================

'use strict';

class Js8CallDecoder {
  constructor() {
    this._worker      = null;
    this._running     = false;
    this._wasmReady   = false;
    this._modeId      = 1;     // Normal by default
    this._decodeCount = 0;
    this._onMessage   = null;
    this._onStatus    = null;
  }

  // ----------------------------------------------------------
  // init(onMessage, onStatus) — spawn Worker, wait for ready
  // ----------------------------------------------------------
  async init(onMessage, onStatus) {
    this._onMessage = onMessage;
    this._onStatus  = onStatus;

    this._worker = new Worker('/js/js8call-worker.js', { type: 'module' });

    this._worker.onerror = (e) => {
      console.error('[JS8] Worker error:', e);
      this._onStatus?.('Worker error: ' + e.message);
    };

    return new Promise((resolve) => {
      this._worker.onmessage = (e) => {
        if (e.data.type === 'ready') {
          this._wasmReady = e.data.wasm;
          this._worker.onmessage = (ev) => this._handleWorkerMsg(ev.data);
          this._onStatus?.(e.data.msg ||
            (e.data.wasm ? '✓ JS8Call WASM ready' : '⚠ WASM not loaded — energy fallback'));
          resolve(e.data);
        }
      };
    });
  }

  // ----------------------------------------------------------
  // start(modeId) — begin accumulation and decode cycle
  // ----------------------------------------------------------
  start(modeId) {
    if (!this._worker) return;
    this._modeId  = modeId ?? this._modeId;
    this._running = true;
    this._worker.postMessage({
      type:    'start',
      mode:    this._modeId,
      freqMin: 200,
      freqMax: 3000,
    });
    const labels = ['Slow (30 s)', 'Normal (15.6 s)', 'Fast (10 s)', 'Turbo (6 s)'];
    this._onStatus?.('Buffering — ' + (labels[this._modeId] ?? 'Unknown') + ' mode…');
    console.log('[JS8] Started, mode id:', this._modeId);
  }

  // ----------------------------------------------------------
  // stop()
  // ----------------------------------------------------------
  stop() {
    this._running = false;
    this._worker?.postMessage({ type: 'stop' });
    this._onStatus?.('Stopped.');
  }

  // ----------------------------------------------------------
  // setMode(modeId) — switch speed without restarting Worker
  // ----------------------------------------------------------
  setMode(modeId) {
    this._modeId = modeId;
    if (this._running) {
      this._worker?.postMessage({ type: 'set-mode', mode: modeId });
      const labels = ['Slow (30 s)', 'Normal (15.6 s)', 'Fast (10 s)', 'Turbo (6 s)'];
      this._onStatus?.('Mode → ' + (labels[modeId] ?? 'Unknown'));
    }
  }

  // ----------------------------------------------------------
  // pushAudioFrame(arrayBuffer)
  // Frame format: [4 bytes uint32 timestamp LE][int16[] PCM @ 12 kHz]
  // The buffer is NOT transferred (structured-clone copy) so that
  // other consumers (audioPlayer, ft8Decoder, cwDecoder) can still
  // read the same ArrayBuffer on the main thread.
  // ----------------------------------------------------------
  pushAudioFrame(arrayBuffer) {
    if (!this._running || !this._worker) return;
    this._worker.postMessage({ type: 'audio', buffer: arrayBuffer });
  }

  // ----------------------------------------------------------
  // _handleWorkerMsg(data)
  // ----------------------------------------------------------
  _handleWorkerMsg(data) {
    switch (data.type) {
      case 'decoded':
        this._decodeCount++;
        this._onMessage?.(data.messages, data.slotTime);
        this._onStatus?.(
          data.messages.length > 0
            ? `${data.messages.length} message(s) @ ${data.slotTime}`
            : `No signals @ ${data.slotTime}`
        );
        break;

      case 'error':
        console.error('[JS8] Decode error:', data.msg);
        this._onStatus?.('Error: ' + data.msg);
        break;
    }
  }

  get isRunning()   { return this._running; }
  get wasmReady()   { return this._wasmReady; }
  get decodeCount() { return this._decodeCount; }
}

const js8Decoder = new Js8CallDecoder();
