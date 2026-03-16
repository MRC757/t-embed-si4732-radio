// ============================================================
// FFTProcessor.cpp
// Uses esp-dsp: https://github.com/espressif/esp-dsp
// Add to platformio.ini lib_deps if needed:
//   espressif/esp-dsp@^1.3.5
// For now uses a software FFT fallback compatible with arduino-esp32.
// ============================================================
#include "FFTProcessor.h"
#include "../audio/AudioCapture.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <math.h>

static const char* TAG = "FFT";

// ============================================================
// Software Cooley-Tukey FFT (in-place, radix-2)
// Replace with esp_dsp_fft2r_fc32 for hardware acceleration.
// ============================================================
static void fft_inplace(float* re, float* im, int n) {
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    // FFT butterfly
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * M_PI / len;
        float wRe = cosf(ang), wIm = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float curRe = 1.0f, curIm = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                float uRe = re[i+j],       uIm = im[i+j];
                float vRe = re[i+j+len/2], vIm = im[i+j+len/2];
                float tRe = curRe*vRe - curIm*vIm;
                float tIm = curRe*vIm + curIm*vRe;
                re[i+j]       = uRe + tRe; im[i+j]       = uIm + tIm;
                re[i+j+len/2] = uRe - tRe; im[i+j+len/2] = uIm - tIm;
                float newRe = curRe*wRe - curIm*wIm;
                curIm = curRe*wIm + curIm*wRe;
                curRe = newRe;
            }
        }
    }
}

// ============================================================
// Constructor / begin
// ============================================================
FFTProcessor::FFTProcessor()
    : _fftReal(nullptr), _fftImag(nullptr), _window(nullptr)
    , _magnitudes(nullptr), _sampleBuf(nullptr), _rowReady(false)
    , _noiseFloor(-80.0f), _samplesAccumulated(0)
    , _taskHandle(nullptr)
{
    _rowMutex = xSemaphoreCreateMutex();
    memset(&_latestRow, 0, sizeof(_latestRow));
}

bool FFTProcessor::begin() {
    // Allocate FFT buffers in PSRAM
    size_t fftBytes = FFT_SIZE * sizeof(float);
    _fftReal    = (float*) heap_caps_malloc(fftBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _fftImag    = (float*) heap_caps_malloc(fftBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _window     = (float*) heap_caps_malloc(fftBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _magnitudes = (float*) heap_caps_malloc(fftBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _sampleBuf  = (int16_t*) heap_caps_malloc(FFT_SIZE * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!_fftReal || !_fftImag || !_window || !_magnitudes || !_sampleBuf) {
        ESP_LOGE(TAG, "PSRAM allocation failed for FFT buffers");
        return false;
    }

    _computeHannWindow();

    xTaskCreatePinnedToCore(
        dspTask, "FFTDsp", STACK_DSP, this, TASK_PRIO_DSP, &_taskHandle, CORE_RADIO
    );

    ESP_LOGI(TAG, "FFT processor ready. Size=%d, BinWidth=%.1f Hz, Cols=%d",
             FFT_SIZE, (float)AUDIO_SAMPLE_RATE_HZ / FFT_SIZE, WATERFALL_COLS);
    return true;
}

// ============================================================
// _computeHannWindow()
// ============================================================
void FFTProcessor::_computeHannWindow() {
    for (int i = 0; i < FFT_SIZE; i++) {
        _window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (FFT_SIZE - 1)));
    }
}

// ============================================================
// dspTask / _dspLoop
// ============================================================
void FFTProcessor::dspTask(void* arg) {
    ((FFTProcessor*)arg)->_dspLoop();
}

void FFTProcessor::_dspLoop() {
    // _sampleBuf is allocated in PSRAM in begin() — no internal SRAM used.
    // Previously this was: static int16_t sampleBuf[FFT_SIZE]
    // which permanently consumed 1KB of internal SRAM from the task stack.
    int16_t* sampleBuf = _sampleBuf;

    // Target: 10 waterfall frames per second
    // At 16kHz with 512-sample FFT: each frame covers 32ms
    // We update every ~100ms (3 FFT frames worth of overlap)
    const TickType_t waitTicks = pdMS_TO_TICKS(20);

    while (true) {
        // Accumulate FFT_SIZE samples
        size_t got = audioCapture.read(sampleBuf, FFT_SIZE, waitTicks);
        if (got < FFT_SIZE / 2) {
            // Not enough data yet
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Zero-pad if we got a partial block
        if (got < (size_t)FFT_SIZE) {
            memset(sampleBuf + got, 0, (FFT_SIZE - got) * sizeof(int16_t));
        }

        // Convert int16 to float, apply Hann window
        for (int i = 0; i < FFT_SIZE; i++) {
            _fftReal[i] = (sampleBuf[i] / 32768.0f) * _window[i];
            _fftImag[i] = 0.0f;
        }

        // Compute FFT
        fft_inplace(_fftReal, _fftImag, FFT_SIZE);

        // Compute magnitude spectrum (first half only — FFT_SIZE/2 bins)
        // Bin 0 = DC, Bin FFT_SIZE/2-1 = Nyquist-1
        int halfN = FFT_SIZE / 2;
        for (int i = 0; i < halfN; i++) {
            _magnitudes[i] = _magnitudedB(_fftReal[i], _fftImag[i]);
        }

        // Select WATERFALL_COLS bins from the audio spectrum.
        // We typically want 0–4kHz for SSB/AM (columns 0–128 at 16kHz/512)
        // or 0–8kHz for full audio. Select all halfN bins and decimate.
        WaterfallRow row;
        row.timestampMs = millis();
        _normalizeBins(_magnitudes, row.bins, halfN);

        // Update noise floor estimate (moving minimum)
        for (int i = 0; i < WATERFALL_COLS; i++) {
            if (row.bins[i] < _noiseFloor) {
                _noiseFloor = _noiseFloor * 0.99f + row.bins[i] * 0.01f;
            }
        }

        xSemaphoreTake(_rowMutex, portMAX_DELAY);
        memcpy(&_latestRow, &row, sizeof(WaterfallRow));
        _rowReady = true;
        xSemaphoreGive(_rowMutex);
    }
}

// ============================================================
// _magnitudedB()
// ============================================================
float FFTProcessor::_magnitudedB(float re, float im) {
    float mag = sqrtf(re * re + im * im) / (FFT_SIZE / 2);
    if (mag < 1e-10f) mag = 1e-10f;
    return 20.0f * log10f(mag);
}

// ============================================================
// _normalizeBins() — map halfN bins → WATERFALL_COLS output bins
// Uses linear interpolation / averaging for decimation.
// ============================================================
void FFTProcessor::_normalizeBins(float* mags, float* out, int inCount) {
    float step = (float)inCount / WATERFALL_COLS;
    for (int i = 0; i < WATERFALL_COLS; i++) {
        float start = i * step;
        float end   = start + step;
        int   iStart = (int)start;
        int   iEnd   = min((int)ceil(end), inCount);
        float maxVal = -120.0f;
        for (int j = iStart; j < iEnd; j++) {
            if (mags[j] > maxVal) maxVal = mags[j];
        }
        // Clamp to [-80, 0] dB and normalise to [0, 1]
        float clamped = constrain(maxVal, -80.0f, 0.0f);
        out[i] = (clamped + 80.0f) / 80.0f;  // 0.0 = noise, 1.0 = strong signal
    }
}

bool FFTProcessor::getWaterfallRow(WaterfallRow& out) {
    xSemaphoreTake(_rowMutex, portMAX_DELAY);
    bool ready = _rowReady;
    if (ready) {
        memcpy(&out, &_latestRow, sizeof(WaterfallRow));
        _rowReady = false;
    }
    xSemaphoreGive(_rowMutex);
    return ready;
}

void FFTProcessor::resetNoiseFloor() {
    _noiseFloor = -80.0f;
}

FFTProcessor fftProcessor;
