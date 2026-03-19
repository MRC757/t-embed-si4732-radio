// ============================================================
// audio.js — Web Audio API PCM player with software SSB demod
//
// The ESP32 applies the software product detector before
// streaming (SoftSSBDemod.cpp), so for normal playback the
// audio arrives already demodulated.
//
// The AudioWorklet here adds a SECOND, browser-side BFO stage
// that the user can adjust in real-time to fine-tune pitch
// without round-tripping to the device.  This is identical in
// principle to the ESP32-side product detector — it is an
// independent, low-latency layer for perceptual pitch tuning.
//
// Signal chain:
//   WebSocket int16 PCM (ESP32-demodulated)
//     → SharedArrayBuffer ring
//     → AudioWorklet PCMPlayerProcessor
//         ├── [optional] browser BFO mix + LPF
//         └── float32 output → speakers
//
// The browser BFO only activates when the user adjusts the
// "fine pitch" slider in the UI.  At 0 Hz trim it is a no-op.
// ============================================================

'use strict';

const WORKLET_CODE = `
class PCMPlayerProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this._initialized = false;

    // Browser-side BFO (pitch fine-tune)
    this._bfoEnabled  = false;
    this._bfoPhase    = 0.0;
    this._bfoPhaseInc = 0.0;  // = 2π × bfoHz / sampleRate
    this._lpfZ1       = 0.0;
    this._lpfZ2       = 0.0;
    // One-pole LPF alpha for ~2.4 kHz cutoff at 12 kHz
    // α = exp(-2π × fc / fs) = exp(-2π × 2400/12000) ≈ 0.2846
    this._lpfAlpha    = 0.2846;

    this.port.onmessage = (e) => {
      if (e.data.type === 'init') {
        this._sab      = e.data.sab;
        this._writePos = new Int32Array(this._sab, 0, 1);
        this._readPos  = new Int32Array(this._sab, 4, 1);
        this._buf      = new Int16Array(this._sab, 8);
        this._capacity = this._buf.length;
        this._initialized = true;
      } else if (e.data.type === 'bfo') {
        // Real-time BFO update from main thread — no allocation
        const hz = e.data.hz || 0;
        if (Math.abs(hz) < 5) {
          this._bfoEnabled  = false;
          this._bfoPhaseInc = 0.0;
        } else {
          this._bfoEnabled  = true;
          // sampleRate is a global in the AudioWorkletGlobalScope
          this._bfoPhaseInc = (2 * Math.PI * hz) / sampleRate;
        }
      }
    };
  }

  process(inputs, outputs) {
    if (!this._initialized) return true;
    const out = outputs[0][0];
    const len = out.length;

    for (let i = 0; i < len; i++) {
      // Read from ring buffer
      const wp    = Atomics.load(this._writePos, 0);
      const rp    = Atomics.load(this._readPos,  0);
      const avail = (wp - rp + this._capacity) % this._capacity;

      let sample = avail > 0
        ? this._buf[rp % this._capacity] / 32768.0
        : 0.0;

      if (avail > 0) {
        Atomics.store(this._readPos, 0, (rp + 1) % this._capacity);
      }

      // Browser-side BFO pitch trim (optional, 0 = passthrough)
      if (this._bfoEnabled) {
        const bfoCos  = Math.cos(this._bfoPhase);
        sample        = sample * bfoCos * 2.0;
        this._bfoPhase += this._bfoPhaseInc;
        if (this._bfoPhase > 6.2832) this._bfoPhase -= 6.2832;

        // One-pole LPF: y[n] = α·y[n-1] + (1-α)·x[n]
        this._lpfZ1 = this._lpfAlpha * this._lpfZ1 + (1 - this._lpfAlpha) * sample;
        sample = this._lpfZ1;
      }

      // Clamp and output
      out[i] = Math.max(-1.0, Math.min(1.0, sample));
    }
    return true;
  }
}
registerProcessor('pcm-player', PCMPlayerProcessor);
`;

// ============================================================
// AudioPlayer
// ============================================================
class AudioPlayer {
  constructor() {
    this._ctx        = null;
    this._worklet    = null;
    this._running    = false;
    this._sab        = null;
    this._writePos   = null;
    this._buf        = null;
    this._capacity   = 32768;   // ~2.7 s at 12 kHz
    this._sampleRate = 12000;
    this._latencyMs  = 0;
    this._framesRx   = 0;
  }

  async start() {
    if (this._running) return;

    this._ctx = new (window.AudioContext || window.webkitAudioContext)({
      sampleRate:  this._sampleRate,
      latencyHint: 'interactive',
    });

    // SharedArrayBuffer ring: [4B writePos][4B readPos][capacity × 2B int16]
    const sabSize   = 8 + this._capacity * 2;
    this._sab       = new SharedArrayBuffer(sabSize);
    this._writePos  = new Int32Array(this._sab, 0, 1);
    this._readPos   = new Int32Array(this._sab, 4, 1);
    this._buf       = new Int16Array(this._sab, 8);

    const blob    = new Blob([WORKLET_CODE], { type: 'application/javascript' });
    const blobURL = URL.createObjectURL(blob);
    await this._ctx.audioWorklet.addModule(blobURL);
    URL.revokeObjectURL(blobURL);

    this._worklet = new AudioWorkletNode(this._ctx, 'pcm-player', {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [1],
    });
    this._worklet.port.postMessage({ type: 'init', sab: this._sab });
    this._worklet.connect(this._ctx.destination);

    this._running = true;
    console.log('[Audio] Started @ ' + this._sampleRate + ' Hz');
    return true;
  }

  stop() {
    if (!this._running) return;
    this._worklet?.disconnect();
    this._worklet = null;
    this._ctx?.close();
    this._ctx     = null;
    this._running = false;
    console.log('[Audio] Stopped.');
  }

  // Set browser-side BFO pitch trim (Hz).
  // 0 = disable browser BFO (use ESP32 demodulation only).
  setBrowserBFO(hz) {
    this._worklet?.port.postMessage({ type: 'bfo', hz });
  }

  // Push a binary audio frame from WebSocket.
  // Frame: [4 bytes uint32 timestamp LE][N × int16 PCM @ 16 kHz]
  pushFrame(arrayBuffer) {
    if (!this._running) return;

    const samples = new Int16Array(arrayBuffer, 4);
    const count   = samples.length;

    this._framesRx++;

    const wp   = Atomics.load(this._writePos, 0);
    const rp   = Atomics.load(this._readPos,  0);
    const fill = (wp - rp + this._capacity) % this._capacity;
    this._latencyMs = Math.round((fill / this._sampleRate) * 1000);

    for (let i = 0; i < count; i++) {
      const nextWp = (wp + i + 1) % this._capacity;
      if (nextWp === rp) break;  // ring full — drop
      this._buf[(wp + i) % this._capacity] = samples[i];
    }
    Atomics.store(this._writePos, 0, (wp + count) % this._capacity);
  }

  get isRunning() { return this._running; }
  get latencyMs() { return this._latencyMs; }
  get framesRx()  { return this._framesRx; }
}

const audioPlayer = new AudioPlayer();
