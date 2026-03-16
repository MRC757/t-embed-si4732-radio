#pragma once
// ============================================================
// FFTProcessor.h — Real-time FFT for spectrum / waterfall
// Uses ESP-DSP hardware-accelerated FFT (esp_dsp library).
// Consumes samples from AudioCapture ring buffer.
// Produces magnitude bins for the web waterfall display.
// ============================================================
#include <Arduino.h>
#include "../config/PinConfig.h"

// Number of magnitude output bins sent to the browser.
// WATERFALL_COLS bins are selected from the lower half of the FFT.
// At 16kHz sample rate, FFT_SIZE/2 bins cover 0–8kHz.
// Each bin width = sample_rate / FFT_SIZE = 16000 / 512 = ~31.25 Hz/bin.

struct WaterfallRow {
    float bins[WATERFALL_COLS];  // Magnitude in dB, range ~-80 to 0 dB
    uint32_t timestampMs;
};

class FFTProcessor {
public:
    FFTProcessor();
    bool begin();

    // Process one FFT frame. Returns true if a new waterfall row is ready.
    // Call this from a dedicated DSP task.
    bool process();

    // Get latest waterfall row (copies to caller's buffer)
    bool getWaterfallRow(WaterfallRow& out);

    // Noise floor calibration
    void  resetNoiseFloor();
    float getNoiseFloor() const { return _noiseFloor; }

    // FreeRTOS task entry
    static void dspTask(void* arg);

private:
    float*  _fftReal;    // Real part (input samples + FFT output) — PSRAM
    float*  _fftImag;    // Imaginary part — PSRAM
    float*  _window;     // Hann window coefficients — PSRAM
    float*  _magnitudes; // Magnitude buffer — PSRAM

    WaterfallRow  _latestRow;
    SemaphoreHandle_t _rowMutex;
    bool          _rowReady;

    float         _noiseFloor;
    uint32_t      _samplesAccumulated;

    TaskHandle_t  _taskHandle;

    // Sample input buffer — allocated in PSRAM in begin()
    // Replaces the static int16_t sampleBuf[FFT_SIZE] that was
    // previously in _dspLoop() and consumed internal SRAM permanently.
    int16_t*      _sampleBuf;

    void  _computeHannWindow();
    void  _applyWindow();
    float _magnitudedB(float re, float im);
    void  _normalizeBins(float* mags, float* out, int count);
    void  _dspLoop();
};

extern FFTProcessor fftProcessor;
