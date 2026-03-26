#pragma once
// ============================================================
// RadioController.h — SI4732 radio control (no SSB patch)
//
// SSB/CW reception uses AM mode on the SI4732 chip with the
// software product detector (SoftSSBDemod) in the DSP pipeline.
// No binary patch is loaded, downloaded, or relied upon.
// ============================================================
#include <Arduino.h>
#include <SI4735.h>
#include <Wire.h>
#include "BandConfig.h"

struct RadioStatus {
    uint32_t  frequencyKHz;    // SI4732 chip tuning frequency (kHz)
    uint32_t  displayFreqHz;   // Combined display frequency (Hz):
                               //   SSB/CW: dialKHz×1000 + bfoAdjHz
                               //   FM:     freqKHz × 10000
                               //   Others: freqKHz × 1000
    DemodMode mode;
    int       bandIndex;
    uint8_t   rssi;
    uint8_t   rssiPeak;        // Peak-hold RSSI (decays ~1 dB/s after 3 s hold)
    uint8_t   snr;
    bool      stereo;
    int8_t    agcGain;
    bool      agcEnabled;
    uint8_t   volume;
    int       bfoHz;           // Current BFO Hz (product detector frequency)
    int       dialKHz;         // User-facing dial frequency (kHz)
    char      rdsStationName[9];
    char      rdsProgramInfo[65];
    // Battery fields — populated from PowerManager (BQ27220/BQ25896)
    // These mirror PowerStatus for convenient broadcast in WebSocketHandler.
    float     batteryVolts;     // V from BQ27220 (or BQ25896 ADC fallback)
    int       batteryPercent;   // 0-100% from BQ27220 (-1 if unavailable)
    bool      isCharging;       // from BQ25896 / BQ27220 current sign
    bool      isUsbConnected;   // VBUS present
    uint32_t  i2cErrors;
};

// A single memory channel slot (populated by getMemorySlot())
struct MemorySlot {
    bool      valid;
    int       slot;
    uint32_t  freqKHz;
    DemodMode mode;
    int       bandIndex;
    char      name[32];
};

class RadioController {
public:
    RadioController();

    bool  begin();
    void  update();

    // Tuning — dialKHz is the HAM/broadcast dial frequency.
    // For LSB the chip is tuned to (dialKHz + lsbTuneOffsetKHz) internally.
    void  setFrequency(uint32_t dialKHz);
    void  stepUp();
    void  stepDown();
    void  seekUp();
    void  seekDown();

    // Band / Mode
    void  setBand(int bandIndex);
    void  setMode(DemodMode mode);
    void  nextBand();
    void  prevBand();

    // Audio
    void  setVolume(uint8_t vol);
    void  volumeUp();
    void  volumeDown();
    void  mute(bool m);

    // BFO — shifts the product detector carrier frequency.
    // Fine-tunes the received pitch for SSB/CW.
    // Range: ±500 Hz around the default BFO for the band.
    void  setBFOTrim(int trimHz);
    void  setBandwidth(uint8_t amBwIdx);

    // AGC
    void  setAGC(bool enable, uint8_t gain = 0);

    // Memory channels (NVS-backed, 10 slots)
    static constexpr int MEM_SLOTS = 10;
    void saveMemory(int slot, const char* label = "");
    bool loadMemory(int slot);
    bool getMemorySlot(int slot, MemorySlot& out) const;

    // Status
    const RadioStatus& getStatus() const { return _status; }
    SI4735&            getSI4735()        { return _radio; }

    void  lockStatus()   { xSemaphoreTake(_statusMutex, portMAX_DELAY); }
    void  unlockStatus() { xSemaphoreGive(_statusMutex); }

private:
    SI4735            _radio;
    RadioStatus       _status;
    SemaphoreHandle_t _statusMutex;

    uint8_t   _currentVolume;
    int       _currentBandIndex;
    DemodMode _currentMode;
    int       _dialKHz;        // current dial frequency
    int       _bfoTrimHz;      // user BFO trim (±500 Hz)

    // S-meter peak hold
    uint8_t   _rssiPeak;
    uint32_t  _rssiPeakDecayMs;

    // SSB merged-frequency state (same as before for smooth encoder tuning)
    int       _ssbDialKHz;     // dial frequency (kHz, integer)
    int       _ssbFineTuneHz;  // sub-kHz BFO fine-tune (±500 Hz)
    static constexpr int SSB_STEP_HZ    = 100;
    static constexpr int SSB_BFO_WINDOW = 500;

    // I2C watchdog
    uint32_t  _i2cErrors;
    uint32_t  _consecutiveErrors;
    // RSSI=0/SNR=0 can mean "no signal" (normal) OR a real I2C failure.
    // A high threshold avoids spurious bus resets when the receiver is
    // simply not picking up any stations (e.g. no antenna connected).
    // 300 × 200ms = 60 s of consistent zeros before concluding bus failure.
    static constexpr uint32_t I2C_RESET_THRESHOLD = 300;

    // Polling timers
    uint32_t  _lastRSSIms, _lastRDSms, _lastBatms;
    static constexpr uint32_t RSSI_POLL_MS = 200;
    static constexpr uint32_t RDS_POLL_MS  = 100;
    static constexpr uint32_t BAT_POLL_MS  = 5000;

    void     _savePrefs();        // update RTC vars + debounced NVS write
    void     _loadPrefs();        // restore from RTC (deep sleep) or NVS

    void     _applyMode(DemodMode mode, uint32_t dialKHz);
    uint32_t _chipFreqForDial(uint32_t dialKHz);
    void  _applyChipTune(uint32_t chipKHz);
    void  _applySSBSoftDemod();
    void  _updateSSBDisplayFreq();
    void  _updateRSSI();
    void  _updateRDS();
    void  _updateBattery();
    void  _resetI2CBus();
    uint32_t _calcDisplayFreqHz() const;
};
