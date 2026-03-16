// ============================================================
// ft8.js — Browser-side FT8 decoder
//
// Key fixes over original:
//   1. Partial-slot alignment: audio buffering only begins at
//      the NEXT complete UTC 15-second boundary after start().
//      The first partial slot is silently discarded.
//      This ensures every submitted window is a full aligned
//      15-second block — misaligned windows produce no decodes.
//
// Architecture:
//   WebSocket audio frames → pushAudioFrame()
//     → discarded until first complete slot boundary
//     → accumulated into Float32Array (_audioBuffer)
//     → at each 15s boundary, submitted to Web Worker
//     → Worker runs ft8_lib WASM decode
//     → decoded messages posted back → displayed in UI
// ============================================================

'use strict';

const AUDIO_RATE  = 16000;

// Slot durations in seconds for each decoder mode.
// FT8 uses the 15-second UTC grid (standard).
// JS8Call supports 6 s (Fast), 10 s (Normal), and 30 s (Slow) variants.
const SLOT_MODES = {
  'FT8':       15,
  'JS8-Fast':   6,
  'JS8-Normal': 10,
  'JS8-Slow':  30,
};

// ============================================================
// Web Worker (inlined as Blob — no separate LittleFS upload)
// ============================================================
const WORKER_CODE = `
'use strict';

let ft8Module = null;
let ft8Ready  = false;

async function loadFT8Lib() {
  try {
    importScripts('/js/ft8_lib.js');
    ft8Module = await FT8Lib();
    ft8Ready  = true;
    postMessage({ type: 'ready', wasm: true });
  } catch (e) {
    ft8Ready = false;
    postMessage({ type: 'ready', wasm: false,
                  msg: 'ft8_lib.wasm not found — energy fallback active' });
  }
}
loadFT8Lib();

function computeEnergydB(samples) {
  let sum = 0;
  for (let i = 0; i < samples.length; i++) sum += samples[i] * samples[i];
  const rms = Math.sqrt(sum / samples.length);
  return rms > 0 ? 20 * Math.log10(rms) : -120;
}

onmessage = function(e) {
  const { type, samples, slotTime, freqHz } = e.data;
  if (type !== 'decode') return;

  if (ft8Ready && ft8Module) {
    try {
      const nSamples  = samples.length;
      const ptr       = ft8Module._malloc(nSamples * 4);
      ft8Module.HEAPF32.set(samples, ptr >> 2);

      const outBufLen = 8192;
      const outPtr    = ft8Module._malloc(outBufLen);
      const nDecoded  = ft8Module._ft8_decode(
        ptr, nSamples, 16000, 100.0, 3000.0, 50, outPtr, outBufLen
      );

      const messages = [];
      if (nDecoded > 0) {
        const raw = ft8Module.UTF8ToString(outPtr);
        for (const line of raw.split('\\n')) {
          const parts = line.trim().split(/\\s+/, 4);
          if (parts.length >= 4) {
            messages.push({
              snr:  parseFloat(parts[0]),
              dt:   parseFloat(parts[1]),
              freq: parseFloat(parts[2]),
              msg:  parts.slice(3).join(' '),
              time: slotTime,
            });
          }
        }
      }
      ft8Module._free(ptr);
      ft8Module._free(outPtr);
      postMessage({ type: 'decoded', messages, slotTime });
    } catch (err) {
      postMessage({ type: 'error', msg: err.message });
    }
  } else {
    const dB = computeEnergydB(samples);
    postMessage({
      type: 'decoded',
      slotTime,
      messages: dB > -60 ? [{
        snr: Math.round(dB + 60), dt: 0, freq: freqHz,
        msg: '(ft8_lib not loaded — energy: ' + dB.toFixed(1) + ' dBFS)',
        time: slotTime,
      }] : [],
    });
  }
};
`;

// ============================================================
// FT8Decoder
// ============================================================
class FT8Decoder {
  constructor() {
    this._worker         = null;
    this._running        = false;
    this._wasmReady      = false;

    // ── Slot mode ─────────────────────────────────────────────
    // Defaults to FT8 (15 s). Call setSlotMode() to switch.
    this._slotSec        = SLOT_MODES['FT8'];
    this._slotSamples    = this._slotSec * AUDIO_RATE;

    // ── NTP clock correction ──────────────────────────────────
    // When a status frame arrives with utcMs, we record the ESP32's
    // UTC epoch ms and the local Date.now() at that instant.
    // _correctedNow() then returns: espUtcMs + (Date.now() - localMs)
    // This corrects for browser clock skew independently of internet access.
    this._ntpRefEspMs   = 0;   // utcMs from last NTP-synced status frame
    this._ntpRefLocalMs = 0;   // Date.now() when that frame was received

    // ── Slot alignment state ─────────────────────────────────
    // When start() is called, we note the NEXT complete UTC boundary
    // (aligned to this._slotSec) and discard all audio until that moment.
    this._waitingForBoundary = false;  // true = discarding pre-boundary audio
    this._firstBoundaryMs    = 0;      // epoch ms of the next UTC boundary

    // ── Audio accumulation ────────────────────────────────────
    this._audioBuffer  = new Float32Array(this._slotSamples);
    this._bufferFill   = 0;
    this._slotStartMs  = 0;    // epoch ms of the slot currently being filled

    this._currentFreq  = 14074;
    this._decodeCount  = 0;
    this._onMessage    = null;
    this._onStatus     = null;
    this._timerHandle  = null;
  }

  // ----------------------------------------------------------
  // setSlotMode(name)
  //   Switch slot duration. 'FT8' | 'JS8-Fast' | 'JS8-Normal' | 'JS8-Slow'
  //   Can be called before or after start(). If the decoder is running,
  //   the change takes effect at the next slot boundary.
  // ----------------------------------------------------------
  setSlotMode(name) {
    const sec = SLOT_MODES[name];
    if (!sec) { console.warn('[FT8] Unknown slot mode:', name); return; }
    this._slotSec     = sec;
    this._slotSamples = sec * AUDIO_RATE;
    this._onStatus?.(`Slot mode → ${name} (${sec}s)`);
    console.log('[FT8] Slot mode:', name, sec + 's');
    // Reset buffers for the new slot size; slot alignment restarts automatically.
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
  //   Called by app.js every time a status frame with utcMs arrives.
  //   espUtcMs — the ESP32's gettimeofday epoch ms (NTP-corrected).
  //   receiveMs — Date.now() at the moment the frame was received.
  // ----------------------------------------------------------
  setNtpReference(espUtcMs, receiveMs) {
    this._ntpRefEspMs   = espUtcMs;
    this._ntpRefLocalMs = receiveMs;
  }

  // ----------------------------------------------------------
  // _correctedNow()
  //   Best estimate of current UTC epoch ms.
  //   Uses NTP reference when available; falls back to Date.now().
  // ----------------------------------------------------------
  _correctedNow() {
    if (this._ntpRefEspMs === 0) return Date.now();
    return this._ntpRefEspMs + (Date.now() - this._ntpRefLocalMs);
  }

  // ----------------------------------------------------------
  // manualSync()
  //   Snaps the slot boundary to the current moment.
  //   Use when you hear the very first tone of an FT8 transmission
  //   and NTP is not available — the decoder will start buffering
  //   from that instant and decode the following 15-second slot.
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
  // init() — create Worker and wait for WASM ready signal
  // ----------------------------------------------------------
  async init(onMessage, onStatus) {
    this._onMessage = onMessage;
    this._onStatus  = onStatus;

    const blob    = new Blob([WORKER_CODE], { type: 'application/javascript' });
    const blobURL = URL.createObjectURL(blob);
    this._worker  = new Worker(blobURL);
    URL.revokeObjectURL(blobURL);

    this._worker.onerror = (e) => {
      console.error('[FT8] Worker error:', e);
      this._onStatus?.('Worker error: ' + e.message);
    };

    // Wait for 'ready' message from worker
    return new Promise((resolve) => {
      this._worker.onmessage = (e) => {
        if (e.data.type === 'ready') {
          this._wasmReady = e.data.wasm;
          this._worker.onmessage = (ev) => this._handleWorkerMsg(ev.data);
          this._onStatus?.(e.data.wasm
            ? '✓ ft8_lib WASM ready'
            : '⚠ ' + (e.data.msg || 'Energy fallback active'));
          resolve(e.data);
        }
      };
    });
  }

  // ----------------------------------------------------------
  // start() — begin the decode cycle
  //
  // Alignment strategy:
  //   Find the NEXT UTC 15s boundary (at least 1s from now to
  //   give WebSocket audio time to catch up).
  //   Set _waitingForBoundary = true.
  //   pushAudioFrame() discards samples until that boundary.
  //   At the boundary, normal slot buffering begins.
  //   First complete decode fires 15 seconds later.
  // ----------------------------------------------------------
  start(currentFreqKHz) {
    if (!this._worker) return;

    this._currentFreq        = currentFreqKHz;
    this._running            = true;
    this._bufferFill         = 0;
    this._audioBuffer        = new Float32Array(this._slotSamples);
    this._waitingForBoundary = true;

    // Next complete UTC slot boundary aligned to this._slotSec, at least 1s away
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

    // 500ms polling interval
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
  // Called by app.js for every WebSocket audio binary frame.
  //
  // Frame layout: [4 bytes timestamp uint32LE][int16[] PCM @ 16kHz]
  //
  // Phase 1 (_waitingForBoundary == true):
  //   Discard samples until Date.now() >= _firstBoundaryMs.
  //   On boundary crossing, switch to Phase 2.
  //
  // Phase 2 (accumulating):
  //   Append float32-converted samples to _audioBuffer.
  //   When buffer is full (FT8_SAMPLES), submit for decode.
  // ----------------------------------------------------------
  pushAudioFrame(arrayBuffer) {
    if (!this._running) return;

    // Phase 1: wait for the slot boundary
    if (this._waitingForBoundary) {
      if (this._correctedNow() < this._firstBoundaryMs) return; // still waiting
      // Boundary reached — switch to accumulation phase
      this._waitingForBoundary = false;
      this._slotStartMs        = this._firstBoundaryMs;
      this._bufferFill         = 0;
      this._onStatus?.('Buffering slot…');
      console.log('[FT8] Slot boundary reached. Buffering started.');
    }

    // Phase 2: accumulate
    const samples = new Int16Array(arrayBuffer, 4); // skip 4-byte timestamp
    const space   = this._slotSamples - this._bufferFill;
    const toCopy  = Math.min(samples.length, space);

    for (let i = 0; i < toCopy; i++) {
      this._audioBuffer[this._bufferFill + i] = samples[i] / 32768.0;
    }
    this._bufferFill += toCopy;

    // If overflow samples remain (frame crossed slot boundary), carry over
    if (toCopy < samples.length && this._bufferFill >= FT8_SAMPLES) {
      // Buffer is full — will be submitted by _tick()
      // Overflow samples are lost (they belong to the next slot and
      // the next pushAudioFrame call will start fresh)
    }
  }

  // ----------------------------------------------------------
  // _tick() — called every 500ms
  // Checks slot boundary and submits full buffers for decode.
  // Also updates the countdown timer display.
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

    const elapsed = (now - this._slotStartMs) / 1000;
    const remaining = Math.max(0, this._slotSec - elapsed);

    document.getElementById('ft8-slot-timer').textContent =
      'T-' + Math.ceil(remaining).toString().padStart(2, '0') + 's';

    // Submit when full slot has elapsed AND buffer has enough samples
    if (elapsed >= this._slotSec && this._bufferFill >= this._slotSamples * 0.8) {
      this._submitDecode();
    }
  }

  // ----------------------------------------------------------
  // _submitDecode() — transfer buffer to Worker and reset
  // ----------------------------------------------------------
  _submitDecode() {
    const slice    = this._audioBuffer.slice(0, this._bufferFill);
    const slotTime = new Date(this._slotStartMs).toISOString().slice(11, 19); // HH:MM:SS

    this._worker.postMessage({
      type:     'decode',
      samples:  slice,
      slotTime,
      freqHz:   this._currentFreq * 1000,
    }, [slice.buffer]);  // Transfer ownership — zero-copy

    this._onStatus?.(`Decoding ${slotTime} (${this._bufferFill} samples)…`);
    console.log(`[FT8] Decode submitted: ${slotTime}, ${this._bufferFill} samples`);

    // Reset for next slot — advance slotStart by exactly one slot duration
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
