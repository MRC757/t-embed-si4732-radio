// ============================================================
// AudioCapture.cpp — ADC Continuous Mode audio from IO17
// Captures SI4732 analog audio output using ESP32-S3 ADC DMA.
// ============================================================
#include "AudioCapture.h"
#include "../dsp/SoftSSBDemod.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#if SPEAKER_SW_PASSTHROUGH
#include <driver/i2s.h>   // ESP-IDF 4.4.x I2S driver
#endif

static const char* TAG = "AudioCapture";

// ADC continuous read buffer — one DMA frame worth of raw results
// Each result is 4 bytes: {data_high[3:0], channel[3:0], unit[1:0], data[11:0]}
// Use SOC_ADC_DIGI_RESULT_BYTES macro; typically 4 bytes per sample on S3.
#define ADC_RESULT_BYTES     4
#define ADC_READ_BUF_SIZE    (AUDIO_DMA_BUF_LEN * ADC_RESULT_BYTES)

// Speaker I2S handle (optional passthrough)
#if SPEAKER_SW_PASSTHROUGH
static i2s_chan_handle_t _spkTx = nullptr;
#endif

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
    memset(&_adcChars, 0, sizeof(_adcChars));
    _ringMutex = xSemaphoreCreateMutex();
    _dataReady = xSemaphoreCreateBinary();
    _taskDone  = xSemaphoreCreateBinary();
    // Abort early rather than crash later with a NULL semaphore handle
    configASSERT(_ringMutex);
    configASSERT(_dataReady);
    configASSERT(_taskDone);
}

// ============================================================
// begin()
// ============================================================
bool AudioCapture::begin() {
    // Allocate ring buffer in PSRAM
    _ringBuf = (int16_t*)heap_caps_malloc(
        AUDIO_RING_CAPACITY * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!_ringBuf) {
        ESP_LOGE(TAG, "PSRAM alloc failed for audio ring buffer");
        return false;
    }
    memset(_ringBuf, 0, AUDIO_RING_CAPACITY * sizeof(int16_t));

    // --- ADC calibration characterisation ---
    // Corrects the ESP32-S3 SAR ADC INL bow (≈±50 LSB uncalibrated).
    // esp_adc_cal_characterize() uses eFuse calibration values when present,
    // otherwise falls back to ADC_VREF_MV (1100 mV).
    esp_adc_cal_value_t calType = esp_adc_cal_characterize(
        ADC_UNIT_1, AUDIO_ADC_ATTEN, ADC_WIDTH_BIT_12,
        ADC_VREF_MV, &_adcChars
    );
    ESP_LOGI(TAG, "ADC cal: %s",
        calType == ESP_ADC_CAL_VAL_EFUSE_VREF   ? "eFuse Vref" :
        calType == ESP_ADC_CAL_VAL_EFUSE_TP     ? "eFuse two-point" :
                                                   "default Vref");

    // --- Configure ADC Continuous Mode (ESP-IDF 4.4.x adc_digi_* API) ---
    // ESP32-S3 ADC1 Channel 6 = IO17 (PIN_SI4732_AUDIO)
    adc_digi_init_config_t init_cfg = {
        .max_store_buf_size = ADC_READ_BUF_SIZE * 4,  // 4 frames deep
        .conv_num_each_intr = ADC_READ_BUF_SIZE,
        .adc1_chan_mask      = BIT(AUDIO_ADC_CHANNEL),
        .adc2_chan_mask      = 0,
    };
    esp_err_t err = adc_digi_initialize(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_digi_initialize failed: %s", esp_err_to_name(err));
        heap_caps_free(_ringBuf);
        _ringBuf = nullptr;
        return false;
    }

    // Configure conversion pattern: single channel, IO17
    adc_digi_pattern_config_t pattern = {
        .atten      = AUDIO_ADC_ATTEN,
        .channel    = (uint8_t)(AUDIO_ADC_CHANNEL & 0x7),
        .unit       = 0,   // ADC_UNIT_1 = 0
        .bit_width  = SOC_ADC_DIGI_MAX_BITWIDTH,
    };

    adc_digi_configuration_t adc_cfg = {
        .conv_limit_en  = false,
        .conv_limit_num = 0,
        .pattern_num    = 1,
        .adc_pattern    = &pattern,
        .sample_freq_hz = AUDIO_SAMPLE_RATE_HZ,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2, // ESP32-S3 format
    };
    err = adc_digi_controller_configure(&adc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_digi_controller_configure failed: %s", esp_err_to_name(err));
        adc_digi_deinitialize();
        heap_caps_free(_ringBuf);
        _ringBuf = nullptr;
        return false;
    }

#if SPEAKER_SW_PASSTHROUGH
    // Configure speaker I2S for software passthrough (optional)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        (i2s_port_t)I2S_PORT_SPEAKER, I2S_ROLE_MASTER
    );
    i2s_new_channel(&chan_cfg, &_spkTx, nullptr);
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_I2S_SPK_BCLK,
            .ws   = (gpio_num_t)PIN_I2S_SPK_WCLK,
            .dout = (gpio_num_t)PIN_I2S_SPK_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv=false, .bclk_inv=false, .ws_inv=false },
        },
    };
    i2s_channel_init_std_mode(_spkTx, &std_cfg);
    i2s_channel_enable(_spkTx);
#endif

    // Start ADC
    err = adc_digi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_digi_start failed: %s", esp_err_to_name(err));
        adc_digi_deinitialize();
        heap_caps_free(_ringBuf);
        _ringBuf = nullptr;
        return false;
    }
    _running = true;

    // Launch capture task on Core 1 (radio/audio core)
    xTaskCreatePinnedToCore(
        captureTask, "AudioADC", STACK_AUDIO, this,
        TASK_PRIO_AUDIO, &_taskHandle, CORE_RADIO
    );

    ESP_LOGI(TAG, "ADC audio capture started. IO17 @ %d Hz, 12-bit",
             AUDIO_SAMPLE_RATE_HZ);
    return true;
}

// ============================================================
// captureTask / _captureLoop
// ============================================================
void AudioCapture::captureTask(void* arg) {
    ((AudioCapture*)arg)->_captureLoop();
}

void AudioCapture::_captureLoop() {
    // Raw ADC DMA read buffer
    static uint8_t  rawBuf[ADC_READ_BUF_SIZE];
    // Converted PCM buffer
    static int16_t  pcmBuf[AUDIO_DMA_BUF_LEN];

    while (_running) {
        uint32_t bytesRead = 0;

        // Block until DMA delivers a full frame (or 50ms timeout)
        esp_err_t ret = adc_digi_read_bytes(rawBuf, ADC_READ_BUF_SIZE,
                                             &bytesRead, 50);

        if (ret == ESP_ERR_TIMEOUT || bytesRead == 0) continue;
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ADC read error: %s", esp_err_to_name(ret));
            continue;
        }

        // Convert raw ADC results to signed int16 PCM.
        // On ESP32-S3 with FORMAT_TYPE2, each result is 4 bytes:
        //   bits[11:0]  = ADC data (12-bit)
        //   bits[15:12] = channel
        //   bits[17:16] = unit
        uint32_t numResults = bytesRead / ADC_RESULT_BYTES;
        uint32_t pcmCount   = 0;

        for (uint32_t i = 0; i < numResults && pcmCount < AUDIO_DMA_BUF_LEN; i++) {
            adc_digi_output_data_t* p = (adc_digi_output_data_t*)&rawBuf[i * ADC_RESULT_BYTES];

            // Verify this result is from our channel
            if (p->type2.channel != (uint8_t)(AUDIO_ADC_CHANNEL & 0x7)) continue;

            uint16_t raw = p->type2.data;
            pcmBuf[pcmCount++] = _adcToSample(raw);
        }

        if (pcmCount == 0) continue;

        // ── Software SSB product detection ───────────────────────
        softSSBDemod.process(pcmBuf, pcmCount);

        // ── Software AGC ─────────────────────────────────────────
        // Applied after SSB demod so the gain tracks the demodulated
        // signal level, not the raw AM envelope.
        _applyAGC(pcmBuf, pcmCount);

#if SPEAKER_SW_PASSTHROUGH
        // Route to speaker I2S
        size_t written;
        i2s_channel_write(_spkTx, pcmBuf, pcmCount * sizeof(int16_t),
                          &written, 0);
#endif

        _writeSamplesToRing(pcmBuf, pcmCount);
        xSemaphoreGive(_dataReady);
    }

    adc_digi_stop();
    adc_digi_deinitialize();
    if (_ringBuf) heap_caps_free(_ringBuf);
    xSemaphoreGive(_taskDone);
    vTaskDelete(NULL);
}

// ============================================================
// read()
// ============================================================
size_t AudioCapture::read(int16_t* buf, size_t maxSamples, TickType_t waitTicks) {
    xSemaphoreTake(_dataReady, waitTicks);

    xSemaphoreTake(_ringMutex, portMAX_DELAY);
    size_t avail  = (_writePos - _readPos) & (AUDIO_RING_CAPACITY - 1);
    size_t toRead = min(avail, maxSamples);
    for (size_t i = 0; i < toRead; i++) {
        buf[i] = _ringBuf[(_readPos + i) & (AUDIO_RING_CAPACITY - 1)];
    }
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
    // Wait for the capture task to exit cleanly (it calls vTaskDelete itself)
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
// _applyAGC()
// Measures the RMS of the current frame, then adjusts _agcGain
// toward (AGC_TARGET_RMS / rms) using separate attack/release
// rates.  Gain is clamped to [AGC_MIN_GAIN, AGC_MAX_GAIN].
// The adjusted gain is then applied in-place to the buffer.
// Called with silence (rms < 10) the gain is held to prevent
// runaway amplification between transmissions.
// ============================================================
void AudioCapture::_applyAGC(int16_t* buf, size_t count) {
    if (!_agcEnabled || count == 0) return;

    // Compute RMS
    float sumSq = 0.0f;
    for (size_t i = 0; i < count; i++) {
        float s = (float)buf[i];
        sumSq += s * s;
    }
    float rms = sqrtf(sumSq / (float)count);

    // Only adjust gain when signal is present (not silence/noise floor)
    if (rms > 10.0f) {
        float desired = AGC_TARGET_RMS / rms;
        float alpha   = (desired < _agcGain) ? AGC_ATTACK : AGC_RELEASE;
        _agcGain += alpha * (desired - _agcGain);
        if (_agcGain < AGC_MIN_GAIN) _agcGain = AGC_MIN_GAIN;
        if (_agcGain > AGC_MAX_GAIN) _agcGain = AGC_MAX_GAIN;
    }

    // Apply gain in-place
    for (size_t i = 0; i < count; i++) {
        int32_t s = (int32_t)((float)buf[i] * _agcGain);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
}

AudioCapture audioCapture;
