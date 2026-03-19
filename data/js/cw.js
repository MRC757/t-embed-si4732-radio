// ============================================================
// cw.js — Browser-side CW (Morse code) decoder
//
// Signal chain:
//   WebSocket int16 PCM (12 kHz, ESP32-demodulated)
//     → pushAudioFrame()
//     → Goertzel filter at BFO pitch (700 Hz default)
//     → Adaptive envelope threshold
//     → Mark/space state machine
//     → Morse character lookup
//     → CW log display
//
// Speed detection is automatic: the decoder measures the
// duration of the first few marks and estimates WPM.  It
// adapts continuously so it works from 5 to 40 WPM without
// any manual setting.
//
// The BFO pitch (default 700 Hz) is updated by app.js
// whenever the user adjusts the BFO slider.
// ============================================================

'use strict';

// ITU Morse code table — key is the dot/dash string, value is the character.
const MORSE_TABLE = {
  '.-': 'A',    '-...': 'B',  '-.-.': 'C',  '-..': 'D',   '.': 'E',
  '..-.': 'F',  '--.': 'G',   '....': 'H',  '..': 'I',    '.---': 'J',
  '-.-': 'K',   '.-..': 'L',  '--': 'M',    '-.': 'N',    '---': 'O',
  '.--.': 'P',  '--.-': 'Q',  '.-.': 'R',   '...': 'S',   '-': 'T',
  '..-': 'U',   '...-': 'V',  '.--': 'W',   '-..-': 'X',  '-.--': 'Y',
  '--..': 'Z',
  '-----': '0', '.----': '1', '..---': '2', '...--': '3', '....-': '4',
  '.....': '5', '-....': '6', '--...': '7', '---..': '8', '----.': '9',
  '.-.-.-': '.', '--..--': ',', '..--..': '?', '-..-.': '/',
  '-....-': '-', '-.--.-': ')', '.----.': "'", '-.-.--': '!',
  '---...': ':', '-.-.-.': ';', '-...-': '=', '.-.-.': '+', '.--.-.': '@',
};

class CWDecoder {
  constructor() {
    this._running     = false;
    this._sampleRate  = 12000;

    // Goertzel block size: 10 ms per block
    this._blockSize   = Math.round(this._sampleRate * 0.01);  // 120 samples
    this._sampleBuf   = new Float32Array(this._blockSize);
    this._bufFill     = 0;

    // BFO frequency to track (updated from UI)
    this._bfoHz       = 700;
    this._updateGoertzelCoeff();

    // Adaptive envelope tracking
    this._levelPeak   = 0.0;   // slow peak follower
    this._levelFloor  = 0.0;   // slow noise floor follower
    this._threshold   = 0.0;   // midpoint of peak/floor

    // Mark/space state machine
    this._inMark      = false;
    this._markBlocks  = 0;   // consecutive on-blocks
    this._spaceBlocks = 0;   // consecutive off-blocks

    // Current character code accumulator ('.' and '-' string)
    this._code        = '';

    // WPM estimation: start at 15 WPM, adapt from observed dit durations
    this._wpm         = 15;

    // Output callbacks
    this._onChar   = null;   // (char, code, wpm) → void
    this._onWord   = null;   // () → void — called on word space
    this._onStatus = null;   // (msg) → void
  }

  // ── Public API ─────────────────────────────────────────────

  start(onChar, onWord, onStatus) {
    this._running   = true;
    this._onChar    = onChar;
    this._onWord    = onWord;
    this._onStatus  = onStatus;
    this._reset();
    this._onStatus?.('CW decoder active — listening at ' + this._bfoHz + ' Hz');
  }

  stop() {
    this._running  = false;
    this._onStatus = null;
  }

  // Called by app.js when the user changes the BFO slider.
  setBfoHz(hz) {
    this._bfoHz = hz || 700;
    this._updateGoertzelCoeff();
  }

  // Feed a WebSocket audio frame (same ArrayBuffer as audioPlayer).
  // Frame layout: [4 bytes uint32 timestamp LE][int16[] PCM @ 12 kHz]
  pushAudioFrame(arrayBuffer) {
    if (!this._running) return;
    const samples = new Int16Array(arrayBuffer, 4);
    for (let i = 0; i < samples.length; i++) {
      this._sampleBuf[this._bufFill++] = samples[i] / 32768.0;
      if (this._bufFill >= this._blockSize) {
        this._processBlock();
        this._bufFill = 0;
      }
    }
  }

  get isRunning() { return this._running; }
  get wpm()       { return Math.round(this._wpm); }

  // ── Private ────────────────────────────────────────────────

  _updateGoertzelCoeff() {
    // Goertzel coefficient: 2 × cos(2π × f / fs)
    this._gCoeff = 2.0 * Math.cos(2.0 * Math.PI * this._bfoHz / this._sampleRate);
  }

  _reset() {
    this._inMark = false; this._markBlocks = 0; this._spaceBlocks = 0;
    this._code = ''; this._bufFill = 0;
    this._levelPeak = 0; this._levelFloor = 0; this._threshold = 0;
  }

  // Run Goertzel on the current block, return normalised magnitude [0..1]
  _goertzel() {
    let s1 = 0.0, s2 = 0.0;
    for (let i = 0; i < this._blockSize; i++) {
      const s = this._sampleBuf[i] + this._gCoeff * s1 - s2;
      s2 = s1; s1 = s;
    }
    // Power = s1² + s2² − gCoeff·s1·s2 ; normalise by block size
    const power = s1 * s1 + s2 * s2 - this._gCoeff * s1 * s2;
    return Math.sqrt(Math.max(0, power)) / this._blockSize;
  }

  _processBlock() {
    const mag = this._goertzel();

    // Slow peak follower (rise fast, decay very slowly)
    if (mag > this._levelPeak) {
      this._levelPeak = mag * 0.6 + this._levelPeak * 0.4;
    } else {
      this._levelPeak *= 0.9995;
    }

    // Slow noise floor follower (only falls, rises slowly when quiet)
    if (mag < this._levelFloor + 0.005 || this._levelFloor === 0) {
      this._levelFloor = mag * 0.05 + this._levelFloor * 0.95;
    }

    // Threshold = midpoint between peak and floor.
    // Require minimum peak before declaring any signal present.
    this._threshold = (this._levelPeak + this._levelFloor) * 0.5;
    const isMark = mag > this._threshold && this._levelPeak > 0.02;

    this._stateMachine(isMark);
  }

  _stateMachine(isMark) {
    // dit duration in 10ms blocks at current WPM estimate
    const ditBlocks = Math.max(1, Math.round(120 / this._wpm));

    if (isMark) {
      if (!this._inMark) {
        // Rising edge: evaluate the preceding space
        if (this._spaceBlocks > 0) {
          if (this._spaceBlocks >= ditBlocks * 5) {
            // Long space → word boundary
            this._flushChar();
            this._onWord?.();
          } else if (this._spaceBlocks >= ditBlocks * 2) {
            // Character space
            this._flushChar();
          }
          // else: inter-element space — do nothing
        }
        this._spaceBlocks = 0;
        this._inMark = true;
      }
      this._markBlocks++;
    } else {
      if (this._inMark) {
        // Falling edge: classify the mark as dit or dah
        if (this._markBlocks >= ditBlocks * 2) {
          this._code += '-';
        } else {
          this._code += '.';
          // Refine WPM estimate from observed dit duration
          const observedWpm = 120 / (this._markBlocks * 10) * 1000;
          this._wpm = this._wpm * 0.85 + observedWpm * 0.15;
          this._wpm = Math.max(5, Math.min(40, this._wpm));
        }
        // Guard: drop impossibly long codes (noise)
        if (this._code.length > 7) this._code = '';
        this._markBlocks = 0;
        this._inMark = false;
      }
      this._spaceBlocks++;
    }
  }

  _flushChar() {
    if (!this._code) return;
    const ch = MORSE_TABLE[this._code] ?? ('?' /* + this._code + '?' */);
    this._onChar?.(ch, this._code, Math.round(this._wpm));
    this._code = '';
  }
}

// ============================================================
// UI wiring
// ============================================================
const cwDecoder = new CWDecoder();

function initCWDecoder() {
  const log      = document.getElementById('cw-log');
  const status   = document.getElementById('cw-status');
  const btnStart = document.getElementById('btn-cw-start');
  const btnStop  = document.getElementById('btn-cw-stop');
  const btnClear = document.getElementById('btn-cw-clear');
  const wpmEl    = document.getElementById('cw-wpm');

  if (!log) return;  // CW section not in HTML

  function appendChar(ch) {
    // Append character to the current line span; wrap on word space
    let line = log.lastElementChild;
    if (!line || line.classList.contains('cw-word-end')) {
      line = document.createElement('div');
      line.className = 'cw-line';
      log.appendChild(line);
      if (log.children.length > 80) log.removeChild(log.firstChild);
    }
    line.textContent += ch;
  }

  btnStart?.addEventListener('click', () => {
    cwDecoder.start(
      (ch, code, wpm) => {
        appendChar(ch);
        if (wpmEl) wpmEl.textContent = wpm + ' WPM';
      },
      () => {
        // Word space — mark the line as ended so next char starts fresh
        const line = log.lastElementChild;
        if (line) line.classList.add('cw-word-end');
      },
      (msg) => { if (status) status.textContent = msg; }
    );
    btnStart.classList.add('hidden');
    btnStop?.classList.remove('hidden');
  });

  btnStop?.addEventListener('click', () => {
    cwDecoder.stop();
    if (status) status.textContent = 'Stopped.';
    if (wpmEl)  wpmEl.textContent  = '';
    btnStop.classList.add('hidden');
    btnStart?.classList.remove('hidden');
  });

  btnClear?.addEventListener('click', () => { log.innerHTML = ''; });
}

document.addEventListener('DOMContentLoaded', initCWDecoder);
