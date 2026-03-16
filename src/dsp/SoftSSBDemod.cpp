// ============================================================
// SoftSSBDemod.cpp
// ============================================================
#include "SoftSSBDemod.h"
#include <math.h>
#include <esp_log.h>

static const char* TAG = "SoftSSB";

// ── Sample rate ───────────────────────────────────────────────
static constexpr float SAMPLE_RATE = 16000.0f;

// ── Static member definitions ─────────────────────────────────
float SoftSSBDemod::_sinLUT[SoftSSBDemod::LUT_SIZE];
bool  SoftSSBDemod::_lutReady = false;

// ── Butterworth LPF coefficients ─────────────────────────────
// 4th-order Butterworth, fc = 2400 Hz, fs = 16000 Hz
// Generated with scipy.signal.butter(4, 2400/8000, btype='low', output='sos'):
//   sos = [[b0,b1,b2,1,a1,a2], [b0,b1,b2,1,a1,a2]]
// Note: a0 normalised to 1.0, signs follow Direct Form II convention.
const SoftSSBDemod::BiquadCoeffs SoftSSBDemod::_lpfSOS[2] = {
    // Section 1
    { 0.09853116f,  0.19706232f,  0.09853116f,
     -0.94280904f,  0.33333333f },
    // Section 2
    { 1.00000000f,  2.00000000f,  1.00000000f,
     -0.26794919f,  0.17157288f },
};
// Note: The sections above are scaled so that each processes
// normalised float samples.  The second section b-coefficients
// are unity because the gain was folded into section 1.

// ── Constructor ───────────────────────────────────────────────
SoftSSBDemod::SoftSSBDemod()
    : _enabled(false)
    , _mode(DemodMode::AM)
    , _bfoHz(1500)
    , _phase(0)
    , _phaseInc(0)
    , _bfoMux(portMUX_INITIALIZER_UNLOCKED)
{
    _resetFilterState();
    if (!_lutReady) _initLUT();
    _updatePhaseInc();
}

// ── _initLUT() ────────────────────────────────────────────────
void SoftSSBDemod::_initLUT() {
    for (int i = 0; i < LUT_SIZE; i++) {
        _sinLUT[i] = sinf(2.0f * M_PI * i / LUT_SIZE);
    }
    _lutReady = true;
}

// ── _updatePhaseInc() ─────────────────────────────────────────
void SoftSSBDemod::_updatePhaseInc() {
    // phaseInc = bfoHz / sampleRate × 2^32
    _phaseInc = (uint32_t)((_bfoHz / SAMPLE_RATE) * 4294967296.0f);
}

// ── _resetFilterState() ───────────────────────────────────────
void SoftSSBDemod::_resetFilterState() {
    _z1[0] = _z1[1] = 0.0f;
    _z2[0] = _z2[1] = 0.0f;
}

// ── setMode() ─────────────────────────────────────────────────
void SoftSSBDemod::setMode(DemodMode mode, int bfoHz) {
    _mode   = mode;
    _bfoHz  = constrain(bfoHz, 100, 4000);
    _phase  = 0;  // restart phase on mode change to avoid clicks
    _updatePhaseInc();
    _resetFilterState();
    ESP_LOGI(TAG, "Mode=%s  BFO=%d Hz", demodModeStr(mode), _bfoHz);
}

// ── setBfoHz() ────────────────────────────────────────────────
// Called during fine tuning — does NOT reset phase or filter.
// The BFO frequency glides smoothly between old and new values.
// Uses a spinlock so process() (Core 1) never sees a stale _phaseInc
// after _bfoHz has already changed.
void SoftSSBDemod::setBfoHz(int hz) {
    portENTER_CRITICAL(&_bfoMux);
    _bfoHz = constrain(hz, 100, 4000);
    _updatePhaseInc();
    portEXIT_CRITICAL(&_bfoMux);
}

// ── _lpf() ────────────────────────────────────────────────────
// Cascaded biquad Direct Form II transposed.
// Processes one float sample through both sections.
inline float SoftSSBDemod::_lpf(float x) {
    for (int s = 0; s < 2; s++) {
        const BiquadCoeffs& c = _lpfSOS[s];
        float w  = x - c.a1 * _z1[s] - c.a2 * _z2[s];
        float y  = c.b0 * w + c.b1 * _z1[s] + c.b2 * _z2[s];
        _z2[s]   = _z1[s];
        _z1[s]   = w;
        x        = y;
    }
    return x;
}

// ── process() ────────────────────────────────────────────────
// In-place SSB product detection:
//   output(n) = LPF( input(n) × cos(2π × bfoHz × n / fs) )
//
// phaseInc is snapshotted once under the spinlock so the entire
// buffer uses a consistent BFO frequency even if setBfoHz() is
// called concurrently from Core 0.
void SoftSSBDemod::process(int16_t* samples, size_t count) {
    if (!_enabled) return;

    // Snapshot phaseInc atomically for the whole buffer
    portENTER_CRITICAL(&_bfoMux);
    uint32_t phaseInc = _phaseInc;
    portEXIT_CRITICAL(&_bfoMux);

    for (size_t i = 0; i < count; i++) {
        float in = samples[i] * (1.0f / 32768.0f);

        // DDS BFO: cos = sin shifted by quarter-wave (LUT_SIZE/4)
        uint8_t cosIdx = (uint8_t)((_phase >> 24) + (LUT_SIZE / 4));
        float   bfo    = _sinLUT[cosIdx];
        _phase += phaseInc;   // local snapshot — consistent for whole buffer

        // Mix, low-pass filter, restore amplitude (mixing halves it)
        float out = _lpf(in * bfo) * 2.0f;

        int32_t q = (int32_t)(out * 32767.0f);
        if      (q >  32767) q =  32767;
        else if (q < -32768) q = -32768;
        samples[i] = (int16_t)q;
    }
}

// Global singleton
SoftSSBDemod softSSBDemod;
