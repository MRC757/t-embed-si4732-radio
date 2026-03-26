// ============================================================
// AudioCapture.cpp — I2S Slave RX (Option 3 — NON-FUNCTIONAL)
//
// Option 3 was tested and FAILED: The SI4732-A10 digital audio pins
// (DOUT/DFS/DCLK) are not wired to IO06/IO05/IO07 on this module PCB.
// i2s_read() times out on every call. No audio data is produced.
//
// This code remains as a reference implementation.
// A hardware modification is needed before audio capture works.
// See AudioCapture.h for tested options and next steps.
//
// If/when wired: SI4732 DOUT/DFS/DCLK → IO06/IO05/IO07 → ESP32 I2S slave.
// SI4732 outputs 48 kHz 16-bit stereo; decimated 4:1 → 12 kHz mono.
// ============================================================
#include "AudioCapture.h"
#include "../dsp/SoftSSBDemod.h"
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char* TAG = "AudioCapture";

// I2S read buffer: 512 output samples × 4 (48→12 kHz) × 2 ch = 4096 int16
#define I2S_RX_SAMPLES   (AUDIO_DMA_BUF_LEN * 4 * 2)   // stereo int16 at 48 kHz
#define I2S_RX_BYTES     (I2S_RX_SAMPLES * sizeof(int16_t))

// ============================================================
// Constructor
// ============================================================
AudioCapture::AudioCapture()
    : _running(false)
    , _gain(1.0f)
    , _droppedSamples(0)
    , _taskHandle(nullptr)
    , _dcX1(0.0f)
    , _dcY1(0.0f)
    , _agcEnabled(true)
    , _agcGain(1.0f)
    , _ringBuf(nullptr)
    , _writePos(0)
    , _readPos(0)
{
    _ringMutex = xSemaphoreCreateMutex();
    _dataReady = xSemaphoreCreateBinary();
    _taskDone  = xSemaphoreCreateBinary();
    configASSERT(_ringMutex);
    configASSERT(_dataReady);
    configASSERT(_taskDone);
}

// ============================================================
// begin() — I2S slave RX init
// ============================================================
bool AudioCapture::begin() {
    // Allocate ring buffer in PSRAM
    _ringBuf = (int16_t*)heap_caps_malloc(
        AUDIO_RING_CAPACITY * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!_ringBuf) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        return false;
    }
    memset(_ringBuf, 0, AUDIO_RING_CAPACITY * sizeof(int16_t));

    // ── I2S slave RX — receives SI4732 digital audio output ──
    // SI4732 is the I2S master; ESP32 listens as slave.
    // BCLK=IO07, LRCLK=IO05, DATA=IO06 (same pins as speaker slot).
    // SI4732 configured for 48 kHz 16-bit stereo by RadioController.
    i2s_config_t i2s_cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX),
        .sample_rate          = I2S_SLAVE_RATE_HZ,   // 48 kHz
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 512,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0,
        .mclk_multiple        = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan         = I2S_BITS_PER_CHAN_16BIT,
    };
    i2s_pin_config_t i2s_pins;
    i2s_pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    i2s_pins.bck_io_num   = PIN_I2S_SPK_BCLK;   // IO07 — BCLK from SI4732
    i2s_pins.ws_io_num    = PIN_I2S_SPK_WCLK;   // IO05 — LRCLK from SI4732
    i2s_pins.data_out_num = I2S_PIN_NO_CHANGE;   // TX unused
    i2s_pins.data_in_num  = PIN_I2S_SPK_DOUT;   // IO06 — DOUT from SI4732

    esp_err_t err = i2s_driver_install((i2s_port_t)I2S_PORT_SPEAKER, &i2s_cfg, 0, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_driver_install failed: %s", esp_err_to_name(err));
        heap_caps_free(_ringBuf); _ringBuf = nullptr;
        return false;
    }
    err = i2s_set_pin((i2s_port_t)I2S_PORT_SPEAKER, &i2s_pins);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_set_pin failed: %s", esp_err_to_name(err));
        i2s_driver_uninstall((i2s_port_t)I2S_PORT_SPEAKER);
        heap_caps_free(_ringBuf); _ringBuf = nullptr;
        return false;
    }

    _running = true;
    xTaskCreatePinnedToCore(
        captureTask, "AudioI2S", STACK_AUDIO, this,
        TASK_PRIO_AUDIO, &_taskHandle, CORE_RADIO
    );

    ESP_LOGI(TAG, "I2S slave RX started: port=%d BCLK=IO%d WS=IO%d DIN=IO%d @ %u Hz",
             I2S_PORT_SPEAKER, PIN_I2S_SPK_BCLK, PIN_I2S_SPK_WCLK,
             PIN_I2S_SPK_DOUT, I2S_SLAVE_RATE_HZ);
    return true;
}

// ============================================================
// captureTask / _captureLoop
// ============================================================
void AudioCapture::captureTask(void* arg) {
    ((AudioCapture*)arg)->_captureLoop();
}

void AudioCapture::_captureLoop() {
    // I2S RX buffer: stereo 16-bit at 48 kHz
    // One frame = 512 output samples × 4 (ratio) × 2 ch = 4096 int16
    static int16_t i2sBuf[I2S_RX_SAMPLES];
    static int16_t pcmBuf[AUDIO_DMA_BUF_LEN];

    while (_running) {
        size_t bytesRead = 0;
        esp_err_t ret = i2s_read((i2s_port_t)I2S_PORT_SPEAKER,
                                  i2sBuf, I2S_RX_BYTES,
                                  &bytesRead, pdMS_TO_TICKS(100));

        if (ret != ESP_OK || bytesRead == 0) continue;

        // Stereo 48 kHz → mono 12 kHz: 4:1 decimation, L channel only.
        // I2S layout: [L0, R0, L1, R1, ...] interleaved int16.
        // Output sample n = i2sBuf[n * 4 * 2] = every 4th L channel.
        uint32_t stereoFrames = bytesRead / (2 * sizeof(int16_t));
        uint32_t pcmCount     = 0;

        for (uint32_t i = 0; i < stereoFrames && pcmCount < AUDIO_DMA_BUF_LEN; i += 4) {
            int16_t raw = i2sBuf[i * 2];   // L channel of every 4th frame

            // IIR DC blocker (handles any residual DC from digital path)
            float x = (float)raw;
            float y = x - _dcX1 + 0.9999f * _dcY1;
            _dcX1 = x;
            _dcY1 = y;

            int32_t s = (int32_t)(y * _gain);
            pcmBuf[pcmCount++] = (int16_t)constrain(s, -32768, 32767);
        }

        if (pcmCount == 0) continue;

        // SSB product detector (SSB/CW modes only)
        softSSBDemod.process(pcmBuf, pcmCount);

        // Software AGC
        _applyAGC(pcmBuf, pcmCount);

        // Diagnostics every ~5 seconds (120 frames × ~42 ms each)
        static uint32_t _diagFrames = 0;
        if (++_diagFrames % 120 == 0) {
            ESP_LOGI(TAG, "AGC gain=%.2f  pcm=%u samples  i2s=%u bytes",
                     _agcGain, (unsigned)pcmCount, (unsigned)bytesRead);
        }

        _writeSamplesToRing(pcmBuf, pcmCount);
        xSemaphoreGive(_dataReady);
    }

    i2s_driver_uninstall((i2s_port_t)I2S_PORT_SPEAKER);
    if (_ringBuf) heap_caps_free(_ringBuf);
    xSemaphoreGive(_taskDone);
    vTaskDelete(NULL);
}

// ============================================================
// read / available / flush / end
// ============================================================
size_t AudioCapture::read(int16_t* buf, size_t maxSamples, TickType_t waitTicks) {
    xSemaphoreTake(_dataReady, waitTicks);
    xSemaphoreTake(_ringMutex, portMAX_DELAY);
    size_t avail  = (_writePos - _readPos) & (AUDIO_RING_CAPACITY - 1);
    size_t toRead = min(avail, maxSamples);
    for (size_t i = 0; i < toRead; i++)
        buf[i] = _ringBuf[(_readPos + i) & (AUDIO_RING_CAPACITY - 1)];
    _readPos = (_readPos + toRead) & (AUDIO_RING_CAPACITY - 1);
    xSemaphoreGive(_ringMutex);
    return toRead;
}

size_t AudioCapture::available() const {
    return (_writePos - _readPos) & (AUDIO_RING_CAPACITY - 1);
}

void AudioCapture::flush() {
    xSemaphoreTake(_ringMutex, portMAX_DELAY);
    _readPos = _writePos;
    xSemaphoreGive(_ringMutex);
}

void AudioCapture::end() {
    if (!_running) return;
    _running = false;
    xSemaphoreTake(_taskDone, pdMS_TO_TICKS(500));
    _taskHandle = nullptr;
}

void AudioCapture::_writeSamplesToRing(const int16_t* samples, size_t count) {
    xSemaphoreTake(_ringMutex, portMAX_DELAY);
    for (size_t i = 0; i < count; i++) {
        size_t nextWrite = (_writePos + 1) & (AUDIO_RING_CAPACITY - 1);
        if (nextWrite == _readPos) {
            _droppedSamples++;
            _readPos = (_readPos + 1) & (AUDIO_RING_CAPACITY - 1);
        }
        _ringBuf[_writePos] = samples[i];
        _writePos = (_writePos + 1) & (AUDIO_RING_CAPACITY - 1);
    }
    xSemaphoreGive(_ringMutex);
}

// ============================================================
// _applyAGC
// ============================================================
void AudioCapture::_applyAGC(int16_t* buf, size_t count) {
    if (!_agcEnabled || count == 0) return;
    float sumSq = 0.0f;
    for (size_t i = 0; i < count; i++) { float s = (float)buf[i]; sumSq += s * s; }
    float rms = sqrtf(sumSq / (float)count);
    if (rms > 10.0f) {
        float desired = AGC_TARGET_RMS / rms;
        float alpha   = (desired < _agcGain) ? AGC_ATTACK : AGC_RELEASE;
        _agcGain += alpha * (desired - _agcGain);
        if (_agcGain < AGC_MIN_GAIN) _agcGain = AGC_MIN_GAIN;
        if (_agcGain > AGC_MAX_GAIN) _agcGain = AGC_MAX_GAIN;
    }
    for (size_t i = 0; i < count; i++) {
        int32_t s = (int32_t)((float)buf[i] * _agcGain);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
}

AudioCapture audioCapture;
