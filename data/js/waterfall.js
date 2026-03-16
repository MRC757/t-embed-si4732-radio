// ============================================================
// waterfall.js — Real-time spectrum waterfall renderer
//
// Receives waterfall rows from WebSocketHandler binary frames:
//   [4 bytes: uint32 magic 0x57465246]
//   [4 bytes: uint32 timestamp ms]
//   [256 × float32: magnitude bins, range 0.0–1.0]
//
// Renders a scrolling spectrogram on a <canvas> element.
// New rows are appended at the top; history scrolls down.
//
// Colour palettes:
//   'heat'  — black → red → yellow → white (classic SDR)
//   'ice'   — black → blue → cyan → white
//   'green' — black → dark green → bright green (night mode)
// ============================================================

'use strict';

const WF_MAGIC = 0x57465246; // "WFRF"

class Waterfall {
  constructor(canvasId) {
    this._canvas    = document.getElementById(canvasId);
    this._ctx       = this._canvas.getContext('2d', { willReadFrequently: true });
    this._enabled   = true;
    this._palette   = 'heat';
    this._cols      = 256;   // bins per row (matches WATERFALL_COLS in PinConfig.h)
    this._rows      = 200;   // visible history rows
    this._zoom      = 1.0;
    this._offset    = 0;     // horizontal pan (future use)
    this._rowBuffer = null;  // ImageData for one row
    this._lut       = null;  // colour lookup table [256 × {r,g,b}]

    this._buildLUT(this._palette);
    this._initCanvas();

    // Crosshair on mouse move
    this._canvas.addEventListener('mousemove', e => this._onMouse(e));
    this._canvas.addEventListener('mouseleave', () => this._clearCrosshair());
  }

  // ----------------------------------------------------------
  // Initialise / resize canvas
  // ----------------------------------------------------------
  _initCanvas() {
    this._canvas.width  = this._cols;
    this._canvas.height = this._rows;
    this._ctx.fillStyle = '#000';
    this._ctx.fillRect(0, 0, this._cols, this._rows);
    this._rowBuffer = this._ctx.createImageData(this._cols, 1);
  }

  // ----------------------------------------------------------
  // Build colour lookup table (256 entries, one per magnitude level)
  // ----------------------------------------------------------
  _buildLUT(palette) {
    this._lut = new Array(256);
    for (let i = 0; i < 256; i++) {
      const t = i / 255;
      let r, g, b;

      switch (palette) {
        case 'heat':
          // Black → dark red → red → orange → yellow → white
          if (t < 0.25) {
            r = Math.round(t * 4 * 180); g = 0; b = 0;
          } else if (t < 0.5) {
            const s = (t - 0.25) * 4;
            r = 180 + Math.round(s * 75); g = Math.round(s * 80); b = 0;
          } else if (t < 0.75) {
            const s = (t - 0.5) * 4;
            r = 255; g = 80 + Math.round(s * 160); b = 0;
          } else {
            const s = (t - 0.75) * 4;
            r = 255; g = 240 + Math.round(s * 15);
            b = Math.round(s * 255);
          }
          break;

        case 'ice':
          // Black → deep blue → cyan → white
          if (t < 0.4) {
            const s = t / 0.4;
            r = 0; g = 0; b = Math.round(s * 220);
          } else if (t < 0.7) {
            const s = (t - 0.4) / 0.3;
            r = 0; g = Math.round(s * 220); b = 220;
          } else {
            const s = (t - 0.7) / 0.3;
            r = Math.round(s * 255); g = 220 + Math.round(s * 35);
            b = 220 + Math.round(s * 35);
          }
          break;

        case 'green':
        default:
          // Black → dark green → bright green
          r = 0;
          g = Math.round(t < 0.5 ? t * 2 * 180 : 180 + (t - 0.5) * 2 * 75);
          b = Math.round(t > 0.7 ? (t - 0.7) / 0.3 * 80 : 0);
          break;
      }

      this._lut[i] = {
        r: Math.min(255, Math.max(0, r)),
        g: Math.min(255, Math.max(0, g)),
        b: Math.min(255, Math.max(0, b)),
      };
    }
  }

  // ----------------------------------------------------------
  // pushRow(arrayBuffer) — called by app.js when a WF frame arrives
  // ----------------------------------------------------------
  pushRow(arrayBuffer) {
    if (!this._enabled) return;

    const dv = new DataView(arrayBuffer);

    // Validate magic
    const magic = dv.getUint32(0, true);
    if (magic !== WF_MAGIC) return;

    // Read float32 bins (skip 8 bytes header)
    const bins = new Float32Array(arrayBuffer, 8, this._cols);

    // Scroll existing image down by 1 pixel
    const imgData = this._ctx.getImageData(0, 0, this._cols, this._rows - 1);
    this._ctx.putImageData(imgData, 0, 1);

    // Paint new row at top
    const rowPixels = this._rowBuffer.data;
    for (let x = 0; x < this._cols; x++) {
      const mag   = Math.max(0, Math.min(1, bins[x]));
      const idx   = Math.round(mag * 255);
      const col   = this._lut[idx];
      const pixel = x * 4;
      rowPixels[pixel]     = col.r;
      rowPixels[pixel + 1] = col.g;
      rowPixels[pixel + 2] = col.b;
      rowPixels[pixel + 3] = 255;
    }
    this._ctx.putImageData(this._rowBuffer, 0, 0);
  }

  // ----------------------------------------------------------
  // Crosshair + frequency tooltip
  // ----------------------------------------------------------
  _onMouse(e) {
    const rect    = this._canvas.getBoundingClientRect();
    const x       = Math.round((e.clientX - rect.left) / rect.width * this._cols);
    const freqHz  = Math.round(x / this._cols * 8000); // 0–8 kHz audio range
    this._canvas.title = `~${freqHz} Hz`;
  }

  _clearCrosshair() {
    this._canvas.title = '';
  }

  // ----------------------------------------------------------
  // Public API
  // ----------------------------------------------------------
  setEnabled(v)  { this._enabled = v; }
  setPalette(p)  { this._palette = p; this._buildLUT(p); }
  clear() {
    this._ctx.fillStyle = '#000';
    this._ctx.fillRect(0, 0, this._cols, this._rows);
  }
}

const waterfall = new Waterfall('waterfall-canvas');
