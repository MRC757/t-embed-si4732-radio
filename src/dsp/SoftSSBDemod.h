#pragma once
// ============================================================
// SoftSSBDemod.h — Software SSB/CW Product Detector
//
// WHY THIS EXISTS
// ---------------
// The SI4732-A10 requires a proprietary binary patch to enable
// its hardware SSB demodulation mode.  That patch is a fragile,
// volatile RAM blob that is wiped on every mode switch and must
// be reloaded from flash over I2C each time.
//
// This module provides SSB and CW reception WITHOUT the patch
// by implementing a software product detector in the ESP32 DSP
// pipeline.  The approach is identical to how discrete-component
// product detectors work in analog radios:
//
//   1.  Tune SI4732 in narrow-band AM mode at the SSB dial
//       frequency.  The chip's AM detector outputs the
//       baseband envelope of the signal.  For SSB, this
//       envelope contains the audio-frequency spectral content
//       of the transmission (300–3000 Hz voice, or narrower
//       for CW/digital).
//
//   2.  Multiply that audio signal by a software-generated
//       carrier (the BFO tone) at frequency bfoHz.
//
//   3.  Low-pass filter the product to remove the 2×bfo image
//       and noise above the passband.
//
// The result is intelligible SSB voice at the correct pitch
// with no patch dependency.
//
// SIGNAL CHAIN
// ------------
//   SI4732 (AM mode, 3 kHz BW) → IO17 ADC (16 kHz, 12-bit)
//         ↓
//   SoftSSBDemod::process(samples, count)
//         ├── × cos(2π · bfoHz · n / 16000)   [DDS BFO]
//         └── LPF (4th-order Butterworth, fc = 2.4 kHz)
//         ↓
//   Demodulated int16 PCM → ring buffer → WebSocket / Speaker
//
// USB vs LSB vs CW
// ----------------
//   USB:  bfoHz ≈ 1500 Hz (shifts voice band to baseband)
//   LSB:  bfoHz ≈ 1500 Hz, SI4732 tuned 2–3 kHz higher than
//         the LSB dial frequency so that the inverted sideband
//         falls in the AM passband correctly.
//         See RadioController::_applySoftSSBTune() for the
//         frequency-offset calculation.
//   CW:   bfoHz ≈ 700 Hz (narrower BW set on AM filter)
//
// USAGE
// -----
//   SoftSSBDemod demod;
//   demod.setMode(DemodMode::USB, 1500);
//   demod.process(samples, count);   // in-place processing
//   demod.setEnabled(false);         // bypass for AM/FM
// ============================================================

#include <Arduino.h>
#include <freertos/portmacro.h>
#include "radio/BandConfig.h"

class SoftSSBDemod {
public:
    SoftSSBDemod();

    // Configure mode and BFO.
    // bfoHz: carrier reinsertion frequency, 200–3000 Hz.
    // Calling this resets the DDS phase accumulator and IIR state.
    void setMode(DemodMode mode, int bfoHz);

    // Enable or disable in-place processing.
    // When disabled, process() is a no-op (AM/FM passthrough).
    void setEnabled(bool en) { _enabled = en; }
    bool isEnabled()   const { return _enabled; }

    // Change BFO frequency without resetting filter state.
    // Safe to call from any task — atomic float store.
    void setBfoHz(int hz);

    // In-place processing: replaces samples[] with demodulated output.
    // count: number of int16 samples (not bytes).
    void process(int16_t* samples, size_t count);

    int     getBfoHz()  const { return _bfoHz; }
    DemodMode getMode() const { return _mode;  }

private:
    // ── State ────────────────────────────────────────────────
    bool      _enabled;
    DemodMode _mode;
    int       _bfoHz;

    // ── DDS oscillator ────────────────────────────────────────
    // Phase accumulator: 32-bit fixed-point, wraps at 2^32.
    // phaseInc = round(bfoHz / sampleRate × 2^32)
    uint32_t  _phase;
    uint32_t  _phaseInc;

    // Spinlock protecting _bfoHz/_phaseInc: setBfoHz() runs on Core 0,
    // process() runs on Core 1.  portENTER_CRITICAL covers both cores.
    portMUX_TYPE _bfoMux;

    // Precomputed cos/sin look-up table (256 entries, quarter-wave)
    static constexpr int LUT_SIZE = 256;
    static float _sinLUT[LUT_SIZE]; // populated once in constructor
    static bool  _lutReady;

    // ── 4th-order Butterworth LPF (cascaded biquad sections) ──
    // Cutoff 2.4 kHz @ 16 kHz sample rate
    // Coefficients precomputed for these fixed parameters:
    //   sos[0]: b0,b1,b2, a1,a2  (section 1)
    //   sos[1]: b0,b1,b2, a1,a2  (section 2)
    // z1[i], z2[i]: section i biquad state
    struct BiquadCoeffs {
        float b0, b1, b2, a1, a2;
    };
    static const BiquadCoeffs _lpfSOS[2]; // precomputed, see .cpp

    float _z1[2];  // biquad section delay state
    float _z2[2];

    // ── Helpers ────────────────────────────────────────────────
    void   _updatePhaseInc();
    void   _resetFilterState();
    float  _lpf(float x);        // apply cascaded biquad LPF
    static void _initLUT();
};

extern SoftSSBDemod softSSBDemod;
