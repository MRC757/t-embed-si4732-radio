// ============================================================
// ft8worker.js — Module Web Worker for FT8 decoding
//
// Loaded as a module worker by ft8.js:
//   new Worker('/js/ft8worker.js', { type: 'module' })
//
// Requires ft8ts.mjs in the same directory on LittleFS.
// Download from: https://github.com/e04/ft8ts
//   npm pack @e04/ft8ts  →  extract dist/ft8ts.mjs
// Place ft8ts.mjs in data/js/ and run: pio run -t uploadfs
//
// If ft8ts.mjs is absent the worker falls back to RMS energy
// reporting (signal presence only, no callsign decoding).
//
// Message protocol (matches ft8.js expectations):
//   Incoming: { type:'decode', samples:Float32Array, slotTime, freqHz }
//   Outgoing: { type:'ready',   wasm:bool, msg:string }
//             { type:'decoded', messages:[], slotTime }
//             { type:'error',   msg:string }
// ============================================================

let decodeFT8 = null;

// Try to import ft8ts. If the file is missing the catch sets
// decodeFT8 = null and the energy fallback is used instead.
try {
  const mod = await import('./ft8ts.mjs');
  decodeFT8 = mod.decodeFT8;
  postMessage({ type: 'ready', wasm: true,
    msg: '✓ ft8ts ready (pure TS, no WASM required)' });
} catch (e) {
  postMessage({ type: 'ready', wasm: false,
    msg: 'ft8ts.mjs not found — energy fallback active. ' +
         'Place ft8ts.mjs in data/js/ and run uploadfs.' });
}

// ── Energy fallback ─────────────────────────────────────────
function energyFallback(samples, slotTime, freqHz) {
  let sum = 0;
  for (let i = 0; i < samples.length; i++) sum += samples[i] * samples[i];
  const dB = 10 * Math.log10(sum / samples.length + 1e-12);
  return dB > -60 ? [{
    snr:  Math.round(dB + 60),
    dt:   0,
    freq: freqHz,
    msg:  '(ft8ts not loaded — signal energy: ' + dB.toFixed(1) + ' dBFS)',
    time: slotTime,
  }] : [];
}

// ── Decode handler ──────────────────────────────────────────
onmessage = function(e) {
  if (e.data.type !== 'decode') return;
  const { samples, slotTime, freqHz } = e.data;

  try {
    let messages;

    if (decodeFT8) {
      const decoded = decodeFT8(samples, {
        sampleRate:    12000,
        freqLow:       100,
        freqHigh:      3000,
        maxCandidates: 300,
        depth:         2,    // 1=fast, 2=BP+OSD (default), 3=deep
      });

      messages = decoded.map(m => ({
        snr:  m.snr,
        dt:   m.dt,
        freq: m.freq,
        msg:  m.msg,
        time: slotTime,
      }));
    } else {
      messages = energyFallback(samples, slotTime, freqHz);
    }

    postMessage({ type: 'decoded', messages, slotTime });

  } catch (err) {
    postMessage({ type: 'error', msg: err.message });
  }
};
