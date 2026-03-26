#pragma once
// ============================================================
// AudioCapture.h — SI4732 Audio Capture (PENDING HARDWARE MOD)
//
// CONFIRMED HARDWARE ARCHITECTURE (T-Embed SI4732):
//
//   SI4732 LOUT ──► analog amp (on SI4732 module) ──► speaker
//                         │
//                    IO17 (MUTE pin: LOW=playing, HIGH=muted)
//                    Driven LOW in main.cpp to unmute speaker.
//
// TESTED AND FAILED OPTIONS:
//
//   Option 1 — IO17 ADC tap: FAILED
//     The SI4732 LOUT DC bias (~1.65 V) reaches the analog amp
//     shutdown pin when IO17 is ADC input (high-Z). 1.65 V trips
//     the amp mute threshold — speaker goes silent. IO17 cannot
//     serve as both mute GPIO output and ADC input simultaneously.
//
//   Option 3 — I2S Slave RX on IO06/IO05/IO07: FAILED
//     The SI4732-A10 has digital audio output pins (DOUT/DFS/DCLK)
//     but they are NOT wired through the Speaker Slot connector to
//     IO06/IO05/IO07 on this module PCB. i2s_read() timed out on
//     every call — no I2S clocks received in 77+ seconds.
//
// CURRENT STATE:
//   This file contains Option 3 I2S slave RX code. It compiles and
//   i2s_driver_install() succeeds, but i2s_read() never returns data.
//   The web audio stream, FFT waterfall, and browser decoders are
//   therefore inactive.
//
// NEXT STEPS (hardware required):
//   Option 2a — GPIO3/4 jumper from SI4732 LOUT tap on module PCB
//               + 100 nF AC-coupling cap. Find test pad with multimeter.
//   Option 4  — Solder SI4732 LOUT through 100 nF to ES7210 MIC3P
//               (IC pin 31). 24-bit I2S, no IO17 conflict.
//
// SIGNAL PROCESSING CHAIN (per frame, when audio source is active):
//   1. i2s_read() — stereo 48 kHz from SI4732 digital output
//   2. 4:1 mono decimation (L channel, 48→12 kHz)
//   3. IIR DC blocker — y[n] = x[n] - x[n-1] + 0.9999*y[n-1]
//   4. SoftSSBDemod.process() — product detector (SSB/CW only)
//   5. _applyAGC() — RMS tracking, attack 0.3, release 0.02
//   6. ring buffer → WebSocket / FFT / CW decoder
// ============================================================
#include <Arduino.h>
#include <driver/i2s.h>
#include "../config/PinConfig.h"

#define SPEAKER_SW_PASSTHROUGH  0   // I2S TX passthrough OFF.
                                    // Speaker driven by SI4732 analog amp directly.
                                    // I2S port used as slave RX for audio capture.

// Ring buffer: ~5.5 seconds at 12 kHz = 65536 samples
static constexpr size_t AUDIO_RING_CAPACITY = 65536; // power of 2

// I2S slave RX: SI4732 digital audio output rate (FM = 48 kHz)
// Decimated 4:1 to AUDIO_SAMPLE_RATE_HZ (12 kHz) in capture loop.
static constexpr uint32_t I2S_SLAVE_RATE_HZ = 48000;

class AudioCapture {
public:
    AudioCapture();

    // Initialise I2S slave RX on SI4732 digital audio pins.
    // Call once from setup() after radioController.begin()
    // (radioController enables the SI4732 digital output).
    bool begin();

    // Stop I2S and free resources.
    void end();

    // Read up to maxSamples signed int16 PCM samples from the ring buffer.
    // Returns number of samples actually read (may be less than maxSamples).
    size_t read(int16_t* buf, size_t maxSamples,
                TickType_t waitTicks = pdMS_TO_TICKS(10));

    // Drain ring buffer without reading
    void flush();

    // Number of samples currently in the ring buffer
    size_t available() const;

    // Set software gain (1.0 = unity, 2.0 = +6 dB)
    void setGain(float gain) { _gain = gain; }

    // Software AGC
    void enableAGC(bool enable) { _agcEnabled = enable; }
    bool agcEnabled() const     { return _agcEnabled; }
    float agcGain()   const     { return _agcGain; }

    bool     isRunning()      const { return _running; }
    uint32_t droppedSamples() const { return _droppedSamples; }

    // FreeRTOS task entry point (internal use)
    static void captureTask(void* arg);

private:
    bool             _running;
    float            _gain;
    uint32_t         _droppedSamples;
    TaskHandle_t     _taskHandle;

    // IIR DC-blocking filter state (still useful for any residual DC)
    float _dcX1;
    float _dcY1;

    // Software AGC state
    bool  _agcEnabled;
    float _agcGain;
    static constexpr float AGC_TARGET_RMS = 3000.0f;
    static constexpr float AGC_ATTACK     = 0.3f;
    static constexpr float AGC_RELEASE    = 0.02f;
    static constexpr float AGC_MIN_GAIN   = 0.25f;
    static constexpr float AGC_MAX_GAIN   = 32.0f;

    void _applyAGC(int16_t* buf, size_t count);

    // Ring buffer (PSRAM)
    int16_t*          _ringBuf;
    volatile size_t   _writePos;
    volatile size_t   _readPos;
    SemaphoreHandle_t _ringMutex;
    SemaphoreHandle_t _dataReady;
    SemaphoreHandle_t _taskDone;

    void _captureLoop();
    void _writeSamplesToRing(const int16_t* samples, size_t count);
};

extern AudioCapture audioCapture;
