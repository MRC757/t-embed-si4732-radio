// ============================================================
// ft8.js — Browser-side FT8 decoder controller
//
// Decoding is delegated to ft8worker.js (module Web Worker)
// which uses the ft8ts pure-TypeScript library.
// No WASM compilation required — just drop ft8ts.mjs in
// data/js/ and run: pio run -t uploadfs
//
// ft8ts supports FT8 and FT4.
// JS8Call slot timing is provided here for aligned buffering,
// but ft8ts cannot decode JS8Call message content.
//
// Architecture:
//   WebSocket audio frames → pushAudioFrame()
//     → discarded until first complete slot boundary
//     → accumulated into Float32Array (_audioBuffer)
//     → at each slot boundary, transferred to ft8worker.js
//     → worker calls decodeFT8(samples, { sampleRate:12000 })
//     → decoded messages posted back → displayed in UI
// ============================================================

'use strict';

const AUDIO_RATE  = 12000;

// Slot durations in seconds for each decoder mode.
// FT8 uses the 15-second UTC grid.
// JS8Call slots are buffered and submitted at the correct boundary,
// but message content cannot be decoded by ft8ts (FT8/FT4 only).
const SLOT_MODES = {
  'FT8':       15,
  'JS8-Fast':   6,
  'JS8-Normal': 10,
  'JS8-Slow':  30,
};

// ============================================================
// FT8Decoder
// ============================================================
class FT8Decoder {
  constructor() {
    this._worker         = null;
    this._running        = false;
    this._wasmReady      = false;  // true when ft8ts loaded successfully

    // ── Slot mode ─────────────────────────────────────────────
    this._slotSec        = SLOT_MODES['FT8'];
    this._slotSamples    = this._slotSec * AUDIO_RATE;

    // ── NTP clock correction ──────────────────────────────────
    this._ntpRefEspMs   = 0;
    this._ntpRefLocalMs = 0;

    // ── Slot alignment state ─────────────────────────────────
    this._waitingForBoundary = false;
    this._firstBoundaryMs    = 0;

    // ── Audio accumulation ────────────────────────────────────
    this._audioBuffer  = new Float32Array(this._slotSamples);
    this._bufferFill   = 0;
    this._slotStartMs  = 0;

    this._currentFreq  = 14074;
    this._decodeCount  = 0;
    this._onMessage    = null;
    this._onStatus     = null;
    this._timerHandle  = null;
  }

  // ----------------------------------------------------------
  // setSlotMode(name)
  // ----------------------------------------------------------
  setSlotMode(name) {
    const sec = SLOT_MODES[name];
    if (!sec) { console.warn('[FT8] Unknown slot mode:', name); return; }
    this._slotSec     = sec;
    this._slotSamples = sec * AUDIO_RATE;
    this._onStatus?.(`Slot mode → ${name} (${sec}s)`);
    console.log('[FT8] Slot mode:', name, sec + 's');
    if (this._running) {
      this._bufferFill  = 0;
      this._audioBuffer = new Float32Array(this._slotSamples);
      this._waitingForBoundary = true;
      const now  = this._correctedNow();
      const slot = Math.ceil((now + 500) / (this._slotSec * 1000));
      this._firstBoundaryMs = slot * this._slotSec * 1000;
      this._slotStartMs     = this._firstBoundaryMs;
    }
  }

  // ----------------------------------------------------------
  // setNtpReference(espUtcMs, receiveMs)
  // ----------------------------------------------------------
  setNtpReference(espUtcMs, receiveMs) {
    this._ntpRefEspMs   = espUtcMs;
    this._ntpRefLocalMs = receiveMs;
  }

  // ----------------------------------------------------------
  // _correctedNow()
  // ----------------------------------------------------------
  _correctedNow() {
    if (this._ntpRefEspMs === 0) return Date.now();
    return this._ntpRefEspMs + (Date.now() - this._ntpRefLocalMs);
  }

  // ----------------------------------------------------------
  // manualSync()
  // ----------------------------------------------------------
  manualSync() {
    const now = this._correctedNow();
    this._waitingForBoundary = false;
    this._firstBoundaryMs    = now;
    this._slotStartMs        = now;
    this._bufferFill         = 0;
    this._audioBuffer        = new Float32Array(this._slotSamples);
    this._onStatus?.('Manual sync — slot boundary set to now, buffering…');
    console.log('[FT8] Manual sync at', new Date(now).toISOString());
  }

  // ----------------------------------------------------------
  // init() — create module Worker, wait for ready signal
  // ----------------------------------------------------------
  async init(onMessage, onStatus) {
    this._onMessage = onMessage;
    this._onStatus  = onStatus;

    // Module worker — ft8worker.js imports ft8ts.mjs relatively
    this._worker = new Worker('/js/ft8worker.js', { type: 'module' });

    this._worker.onerror = (e) => {
      console.error('[FT8] Worker error:', e);
      this._onStatus?.('Worker error: ' + e.message);
    };

    return new Promise((resolve) => {
      this._worker.onmessage = (e) => {
        if (e.data.type === 'ready') {
          this._wasmReady = e.data.wasm;
          this._worker.onmessage = (ev) => this._handleWorkerMsg(ev.data);
          this._onStatus?.(e.data.msg ||
            (e.data.wasm ? '✓ ft8ts ready' : '⚠ ft8ts not loaded — energy fallback'));
          resolve(e.data);
        }
      };
    });
  }

  // ----------------------------------------------------------
  // start()
  // ----------------------------------------------------------
  start(currentFreqKHz) {
    if (!this._worker) return;

    this._currentFreq        = currentFreqKHz;
    this._running            = true;
    this._bufferFill         = 0;
    this._audioBuffer        = new Float32Array(this._slotSamples);
    this._waitingForBoundary = true;

    const now  = this._correctedNow();
    const slot = Math.ceil((now + 1000) / (this._slotSec * 1000));
    this._firstBoundaryMs = slot * this._slotSec * 1000;
    this._slotStartMs     = this._firstBoundaryMs;

    const waitS = ((this._firstBoundaryMs - now) / 1000).toFixed(1);
    this._onStatus?.(
      `Waiting for slot boundary in ${waitS}s — first decode in ~${(parseFloat(waitS) + this._slotSec).toFixed(0)}s`
    );
    console.log('[FT8] Started. First boundary:',
      new Date(this._firstBoundaryMs).toISOString());

    this._timerHandle = setInterval(() => this._tick(), 500);
  }

  // ----------------------------------------------------------
  // stop()
  // ----------------------------------------------------------
  stop() {
    this._running            = false;
    this._waitingForBoundary = false;
    clearInterval(this._timerHandle);
    this._timerHandle  = null;
    this._bufferFill   = 0;
    this._audioBuffer  = new Float32Array(this._slotSamples);
    this._onStatus?.('Stopped.');
    document.getElementById('ft8-slot-timer').textContent = '';
  }

  // ----------------------------------------------------------
  // pushAudioFrame(arrayBuffer)
  // Frame: [4 bytes uint32 timestamp LE][int16[] PCM @ 12 kHz]
  // ----------------------------------------------------------
  pushAudioFrame(arrayBuffer) {
    if (!this._running) return;

    if (this._waitingForBoundary) {
      if (this._correctedNow() < this._firstBoundaryMs) return;
      this._waitingForBoundary = false;
      this._slotStartMs        = this._firstBoundaryMs;
      this._bufferFill         = 0;
      this._onStatus?.('Buffering slot…');
      console.log('[FT8] Slot boundary reached. Buffering started.');
    }

    const samples = new Int16Array(arrayBuffer, 4);
    const space   = this._slotSamples - this._bufferFill;
    const toCopy  = Math.min(samples.length, space);

    for (let i = 0; i < toCopy; i++) {
      this._audioBuffer[this._bufferFill + i] = samples[i] / 32768.0;
    }
    this._bufferFill += toCopy;
  }

  // ----------------------------------------------------------
  // _tick() — 500 ms polling
  // ----------------------------------------------------------
  _tick() {
    if (!this._running) return;
    const now = this._correctedNow();

    if (this._waitingForBoundary) {
      const msLeft = this._firstBoundaryMs - now;
      document.getElementById('ft8-slot-timer').textContent =
        'Wait ' + (msLeft / 1000).toFixed(0) + 's';
      return;
    }

    const elapsed   = (now - this._slotStartMs) / 1000;
    const remaining = Math.max(0, this._slotSec - elapsed);

    document.getElementById('ft8-slot-timer').textContent =
      'T-' + Math.ceil(remaining).toString().padStart(2, '0') + 's';

    if (elapsed >= this._slotSec && this._bufferFill >= this._slotSamples * 0.8) {
      this._submitDecode();
    }
  }

  // ----------------------------------------------------------
  // _submitDecode()
  // ----------------------------------------------------------
  _submitDecode() {
    const slice    = this._audioBuffer.slice(0, this._bufferFill);
    const slotTime = new Date(this._slotStartMs).toISOString().slice(11, 19);

    this._worker.postMessage({
      type:     'decode',
      samples:  slice,
      slotTime,
      freqHz:   this._currentFreq * 1000,
    }, [slice.buffer]);

    this._onStatus?.(`Decoding ${slotTime} (${this._bufferFill} samples)…`);
    console.log(`[FT8] Decode submitted: ${slotTime}, ${this._bufferFill} samples`);

    this._slotStartMs += this._slotSec * 1000;
    this._audioBuffer  = new Float32Array(this._slotSamples);
    this._bufferFill   = 0;
  }

  // ----------------------------------------------------------
  // _handleWorkerMsg()
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
        console.error('[FT8] Decode error:', data.msg);
        this._onStatus?.('Error: ' + data.msg);
        break;
    }
  }

  get isRunning()   { return this._running; }
  get wasmReady()   { return this._wasmReady; }
  get decodeCount() { return this._decodeCount; }
  get ntpSynced()   { return this._ntpRefEspMs > 0; }
}

const ft8Decoder = new FT8Decoder();
