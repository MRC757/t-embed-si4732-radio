#pragma once
// ============================================================
// AudioCapture.h — SI4732 Analog Audio via ESP32-S3 ADC DMA
//
// HARDWARE REALITY (confirmed from T-Embed SI4732 pinmap):
//   SI4732 AUDIO output → IO17 (ADC1_CH6 on ESP32-S3)
//   There are no I2S digital audio pins from the SI4732.
//
// CAPTURE METHOD: ESP32-S3 ADC Continuous Mode (DMA-backed)
//   - esp_adc/adc_continuous.h driver
//   - 12-bit samples at 16kHz
//   - DMA delivers frames of AUDIO_DMA_BUF_LEN samples per callback
//   - Samples are DC-offset corrected (ADC midpoint ~2048 raw counts)
//     and converted to signed int16 for compatibility with:
//       • WebSocket PCM streaming (browser Web Audio API)
//       • FFTProcessor (spectrum / waterfall)
//       • Speaker I2S passthrough (hear the radio locally)
//
// SPEAKER PASSTHROUGH:
//   Captured ADC samples are scaled and written to the speaker
//   I2S bus (IO07/IO05/IO06) for local audio output.
//   The SI4732 analog audio also drives the speaker amplifier
//   directly (hardware path), so software passthrough adds an
//   echo unless the hardware amp is muted via a GPIO.
//   For web streaming the hardware amp path is fine;
//   the software path is only needed if you want to mute/unmute
//   from firmware. Set SPEAKER_SW_PASSTHROUGH 0 to disable.
//
// AUDIO QUALITY NOTE:
//   12-bit ADC at 16kHz is fully adequate for:
//     - SSB voice (300–3000 Hz passband)
//     - FT8 (audio bandwidth ~2.4 kHz)
//     - WSPR, JS8, other digital modes
//     - AM/FM audio (FM via SI4732 has ~15kHz audio BW but
//       16kHz sample rate limits to 8kHz — fine for monitoring)
// ============================================================
#include <Arduino.h>
#include "driver/adc.h"          // ESP-IDF 4.4.x ADC continuous (adc_digi_*)
#include <esp_adc_cal.h>         // ADC nonlinearity calibration
#include "../config/PinConfig.h"

#define SPEAKER_SW_PASSTHROUGH  0   // 1 = route ADC samples to speaker I2S
                                    // 0 = hardware amp path only (recommended)

// Ring buffer: 4 seconds at 16kHz = 64k samples
static constexpr size_t AUDIO_RING_CAPACITY = 65536; // power of 2

// ADC calibration: reference voltage in mV for esp_adc_cal_characterize().
// 1100 mV is the ESP32-S3 default eFuse reference; the driver uses eFuse
// if available, falling back to this value.
static constexpr uint32_t ADC_VREF_MV = 1100;

class AudioCapture {
public:
    AudioCapture();

    // Initialise ADC continuous driver on PIN_SI4732_AUDIO (IO17).
    // Call once from setup() after radioController.begin().
    bool begin();

    // Stop ADC and free resources.
    void end();

    // Read up to maxSamples signed int16 PCM samples from the ring buffer.
    // Samples are DC-corrected, scaled, and ready for WebSocket/FFT.
    // Returns number of samples actually read (may be less than maxSamples).
    // waitTicks: how long to block waiting for data (0 = non-blocking).
    size_t read(int16_t* buf, size_t maxSamples,
                TickType_t waitTicks = pdMS_TO_TICKS(10));

    // Drain ring buffer without reading
    void flush();

    // Number of samples currently in the ring buffer
    size_t available() const;

    // Set software gain applied to ADC samples (1.0 = unity, 2.0 = +6dB)
    void setGain(float gain) { _gain = gain; }

    bool     isRunning()      const { return _running; }
    uint32_t droppedSamples() const { return _droppedSamples; }

    // FreeRTOS task entry point (internal use)
    static void captureTask(void* arg);

private:
    // No handle: ESP-IDF 4.4.x ADC continuous driver is a singleton
    bool                    _running;
    float                   _gain;
    uint32_t                _droppedSamples;
    TaskHandle_t            _taskHandle;

    // ADC calibration characteristics — populated by begin()
    esp_adc_cal_characteristics_t _adcChars;

    // IIR single-pole DC-blocking high-pass filter state
    // Transfer function: y[n] = x[n] - x[n-1] + 0.9999 * y[n-1]
    // Cutoff ≈ 0.16 Hz at 16 kHz — removes DC and 50/60 Hz hum below 1 Hz.
    float _dcX1;
    float _dcY1;

    // Ring buffer (allocated in PSRAM)
    int16_t*          _ringBuf;
    volatile size_t   _writePos;
    volatile size_t   _readPos;
    SemaphoreHandle_t _ringMutex;
    SemaphoreHandle_t _dataReady;
    SemaphoreHandle_t _taskDone;  // given by capture task just before it exits

    void _captureLoop();
    void _writeSamplesToRing(const int16_t* samples, size_t count);

    // Convert raw 12-bit ADC result to calibrated, DC-blocked, signed int16 PCM.
    // Uses esp_adc_cal_raw_to_voltage() to correct the ESP32-S3 ADC INL bow
    // (±50 LSB typical without calibration).
    inline int16_t _adcToSample(uint16_t raw) {
        // Step 1: linearise ADC reading → millivolts (0–3300 mV range)
        uint32_t mV = esp_adc_cal_raw_to_voltage(raw, &_adcChars);

        // Step 2: IIR DC blocker — centre the signal at 0
        // x is the AC-coupled input (centred on 1650 mV midpoint)
        float x = (float)mV - 1650.0f;
        float y = x - _dcX1 + 0.9999f * _dcY1;
        _dcX1 = x;
        _dcY1 = y;

        // Step 3: scale to int16 with software gain
        // 1650.0f maps the full ±3.3V swing to ±full-scale
        int32_t s = (int32_t)(y * _gain * (32767.0f / 1650.0f));
        return (int16_t)constrain(s, -32768, 32767);
    }
};

extern AudioCapture audioCapture;
