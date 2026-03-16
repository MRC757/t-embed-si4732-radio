// ============================================================
// RadioController.cpp — SI4732 control, no SSB patch
// ============================================================
#include "RadioController.h"
#include "../config/PinConfig.h"
#include "../power/PowerManager.h"
#include "../dsp/SoftSSBDemod.h"
#include <esp_log.h>
#include <esp_task_wdt.h>

static const char* TAG = "RadioCtrl";

// ── Constructor ───────────────────────────────────────────────
RadioController::RadioController()
    : _currentVolume(40)
    , _currentBandIndex(0)
    , _currentMode(DemodMode::FM)
    , _dialKHz(10390)
    , _bfoTrimHz(0)
    , _ssbDialKHz(14074)
    , _ssbFineTuneHz(0)
    , _i2cErrors(0)
    , _consecutiveErrors(0)
    , _lastRSSIms(0), _lastRDSms(0), _lastBatms(0)
{
    memset(&_status, 0, sizeof(_status));
    _statusMutex = xSemaphoreCreateMutex();
}

// ============================================================
// begin()
// ============================================================
bool RadioController::begin() {
    ESP_LOGI(TAG, "Powering up SI4732 (no SSB patch mode)...");

    if (PIN_SI4732_PWR >= 0) {
        pinMode(PIN_SI4732_PWR, OUTPUT);
        digitalWrite(PIN_SI4732_PWR, HIGH);
        delay(100);
    }

    Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);
    Wire.setTimeOut(100);  // 100ms max — prevents infinite I2C hangs

    int addr = _radio.getDeviceI2CAddress(PIN_SI4732_RST);
    if (addr == 0) {
        ESP_LOGW(TAG, "I2C scan failed — forcing addr 0x%02X", SI4732_I2C_ADDR);
        _radio.setDeviceI2CAddress(SI4732_I2C_ADDR >> 1);
    } else {
        ESP_LOGI(TAG, "SI4732 at 0x%02X", addr);
    }

    _radio.setMaxDelayPowerUp(500);
    _radio.setMaxDelaySetFrequency(50);

    _radio.setup(PIN_SI4732_RST, POWER_UP_FM);
    _radio.setFM(6400, 10800, 10390, 10);
    delay(200);
    _radio.setVolume(_currentVolume);

    // Software SSB starts disabled; enabled by setMode(LSB/USB/CW)
    softSSBDemod.setEnabled(false);

    lockStatus();
    _status.frequencyKHz  = 10390;
    _status.dialKHz       = 10390;
    _status.displayFreqHz = 10390 * 10000;
    _status.mode          = DemodMode::FM;
    _status.bandIndex     = 0;
    _status.volume        = _currentVolume;
    unlockStatus();

    ESP_LOGI(TAG, "SI4732 ready. FM 103.9 MHz. Software SSB active.");
    return true;
}

// ============================================================
// update()
// ============================================================
void RadioController::update() {
    uint32_t now = millis();
    if (now - _lastRSSIms >= RSSI_POLL_MS) { _lastRSSIms = now; _updateRSSI(); }
    if (now - _lastRDSms  >= RDS_POLL_MS)  { _lastRDSms  = now; if (_currentMode == DemodMode::FM) _updateRDS(); }
    if (now - _lastBatms  >= BAT_POLL_MS)  { _lastBatms  = now; _updateBattery(); }
    esp_task_wdt_reset();
}

// ============================================================
// setFrequency()
// dialKHz: the user-facing frequency (HAM dial, broadcast, etc.)
// For LSB, the chip is tuned to (dialKHz + lsbTuneOffsetKHz)
// so that the lower sideband falls in the AM passband.
// ============================================================
void RadioController::setFrequency(uint32_t dialKHz) {
    const Band& b = BAND_TABLE[_currentBandIndex];
    dialKHz = constrain(dialKHz, b.freqMin, b.freqMax);
    _dialKHz     = (int)dialKHz;
    _ssbDialKHz  = (int)dialKHz;
    _ssbFineTuneHz = 0;

    if (isSoftSSBMode(_currentMode)) {
        _applyChipTune(_chipFreqForDial(dialKHz));
        _applySSBSoftDemod();
    } else if (_currentMode == DemodMode::FM) {
        _radio.setFrequency(dialKHz);
    } else {
        _radio.setFrequency(dialKHz);
    }

    lockStatus();
    _status.frequencyKHz  = (isSoftSSBMode(_currentMode))
                              ? _chipFreqForDial(dialKHz)
                              : dialKHz;
    _status.dialKHz       = (int)dialKHz;
    _status.bfoHz         = softSSBDemod.getBfoHz();
    _status.displayFreqHz = _calcDisplayFreqHz();
    unlockStatus();
}

// ── Helper: compute chip tuning frequency from dial ───────────
uint32_t RadioController::_chipFreqForDial(uint32_t dialKHz) {
    if (_currentMode == DemodMode::LSB) {
        return dialKHz + BAND_TABLE[_currentBandIndex].lsbTuneOffsetKHz;
    }
    return dialKHz;
}

// ── Helper: apply frequency to chip ──────────────────────────
void RadioController::_applyChipTune(uint32_t chipKHz) {
    _radio.setFrequency(chipKHz);
}

// ============================================================
// stepUp / stepDown
// SSB/CW: 100 Hz per step with BFO window wrap (seamless tuning)
// AM/FM/SW: standard band-table step
// ============================================================
void RadioController::stepUp() {
    if (isSoftSSBMode(_currentMode)) {
        // Protect SSB state — may race with WebSocket handler (both Core 0)
        lockStatus();
        _ssbFineTuneHz += SSB_STEP_HZ;
        const Band& b = BAND_TABLE[_currentBandIndex];
        if (_ssbFineTuneHz > SSB_BFO_WINDOW) {
            if (_ssbDialKHz + 1 <= (int)b.freqMax) {
                _ssbDialKHz += 1;
                _ssbFineTuneHz -= 1000;
            } else {
                _ssbFineTuneHz = SSB_BFO_WINDOW;
            }
        }
        uint32_t chipKHz = _chipFreqForDial(_ssbDialKHz);
        unlockStatus();
        _applyChipTune(chipKHz);   // I2C — outside lock
        _updateSSBDisplayFreq();
        _applySSBSoftDemod();
    } else {
        const Band& b = BAND_TABLE[_currentBandIndex];
        uint32_t f = _status.dialKHz + b.stepKHz;
        if (f > b.freqMax) f = b.freqMin;
        setFrequency(f);
    }
}

void RadioController::stepDown() {
    if (isSoftSSBMode(_currentMode)) {
        lockStatus();
        _ssbFineTuneHz -= SSB_STEP_HZ;
        const Band& b = BAND_TABLE[_currentBandIndex];
        if (_ssbFineTuneHz < -SSB_BFO_WINDOW) {
            if (_ssbDialKHz - 1 >= (int)b.freqMin) {
                _ssbDialKHz -= 1;
                _ssbFineTuneHz += 1000;
            } else {
                _ssbFineTuneHz = -SSB_BFO_WINDOW;
            }
        }
        uint32_t chipKHz = _chipFreqForDial(_ssbDialKHz);
        unlockStatus();
        _applyChipTune(chipKHz);   // I2C — outside lock
        _updateSSBDisplayFreq();
        _applySSBSoftDemod();
    } else {
        const Band& b = BAND_TABLE[_currentBandIndex];
        int32_t f = (int32_t)_status.dialKHz - (int32_t)b.stepKHz;
        if (f < (int32_t)b.freqMin) f = b.freqMax;
        setFrequency((uint32_t)f);
    }
}

// ── _applySSBSoftDemod() ──────────────────────────────────────
// Computes the effective BFO = defaultBfoHz + bfoTrim + fineTune
// and updates SoftSSBDemod.
void RadioController::_applySSBSoftDemod() {
    const Band& b = BAND_TABLE[_currentBandIndex];
    int effectiveBfo = b.defaultBfoHz + _bfoTrimHz + _ssbFineTuneHz;
    effectiveBfo = constrain(effectiveBfo, 100, 4000);
    softSSBDemod.setBfoHz(effectiveBfo);
}

// ── _updateSSBDisplayFreq() ───────────────────────────────────
void RadioController::_updateSSBDisplayFreq() {
    const Band& b = BAND_TABLE[_currentBandIndex];
    int effectiveBfo = b.defaultBfoHz + _bfoTrimHz + _ssbFineTuneHz;

    lockStatus();
    _status.dialKHz       = _ssbDialKHz;
    _status.frequencyKHz  = _chipFreqForDial(_ssbDialKHz);
    _status.bfoHz         = effectiveBfo;
    _status.displayFreqHz = (uint32_t)((_ssbDialKHz * 1000) + _ssbFineTuneHz);
    unlockStatus();
}

// ── _calcDisplayFreqHz() ─────────────────────────────────────
uint32_t RadioController::_calcDisplayFreqHz() const {
    if (isSoftSSBMode(_currentMode)) {
        return (uint32_t)((_ssbDialKHz * 1000) + _ssbFineTuneHz);
    }
    if (_currentMode == DemodMode::FM) {
        return _status.frequencyKHz * 10000;
    }
    return _status.dialKHz * 1000;
}

// ============================================================
// seekUp / seekDown (AM/FM only — not useful for SSB)
// ============================================================
void RadioController::seekUp() {
    if (_currentMode == DemodMode::FM) _radio.seekStationUp();
    else _radio.seekStation(SEEK_UP, 1);
    uint16_t f = _radio.getFrequency();
    lockStatus();
    _status.frequencyKHz  = f;
    _status.dialKHz       = f;
    _status.displayFreqHz = _calcDisplayFreqHz();
    unlockStatus();
}

void RadioController::seekDown() {
    if (_currentMode == DemodMode::FM) _radio.seekStationDown();
    else _radio.seekStation(SEEK_DOWN, 1);
    uint16_t f = _radio.getFrequency();
    lockStatus();
    _status.frequencyKHz  = f;
    _status.dialKHz       = f;
    _status.displayFreqHz = _calcDisplayFreqHz();
    unlockStatus();
}

// ============================================================
// setBand()
// ============================================================
void RadioController::setBand(int bandIndex) {
    if (bandIndex < 0 || bandIndex >= BAND_COUNT) return;
    _currentBandIndex = bandIndex;
    const Band& b = BAND_TABLE[bandIndex];
    ESP_LOGI(TAG, "Band: %s  Default: %u kHz", b.name, b.freqDefault);
    _bfoTrimHz = 0;
    _applyMode(b.mode, b.freqDefault);
    lockStatus();
    _status.bandIndex     = bandIndex;
    _status.mode          = b.mode;
    _status.dialKHz       = b.freqDefault;
    _status.frequencyKHz  = _chipFreqForDial(b.freqDefault);
    _status.displayFreqHz = _calcDisplayFreqHz();
    unlockStatus();
}

void RadioController::nextBand() { setBand((_currentBandIndex + 1) % BAND_COUNT); }
void RadioController::prevBand() { setBand((_currentBandIndex - 1 + BAND_COUNT) % BAND_COUNT); }

// ============================================================
// setMode()
// ============================================================
void RadioController::setMode(DemodMode mode) {
    // Snapshot dial frequency safely before releasing lock
    lockStatus();
    uint32_t dialKHz = (uint32_t)_status.dialKHz;
    unlockStatus();

    _applyMode(mode, dialKHz);  // sets _currentMode under lock

    lockStatus();
    _status.mode          = mode;
    _status.displayFreqHz = _calcDisplayFreqHz();
    unlockStatus();
}

// ============================================================
// _applyMode()
// All SSB/CW modes use AM on the chip + SoftSSBDemod.
// The mode switch is always clean — no patch state to track.
// ============================================================
void RadioController::_applyMode(DemodMode mode, uint32_t dialKHz) {
    // Protect private control state — _currentMode is read on Core 1
    // by _updateRSSI(); the SSB variables may be read by stepUp/stepDown.
    lockStatus();
    _currentMode   = mode;
    _dialKHz       = (int)dialKHz;
    _ssbDialKHz    = (int)dialKHz;
    _ssbFineTuneHz = 0;
    unlockStatus();

    const Band& b = BAND_TABLE[_currentBandIndex];

    switch (mode) {
        case DemodMode::FM:
            softSSBDemod.setEnabled(false);
            _radio.setFM(6400, 10800, dialKHz, 10);
            break;

        case DemodMode::AM:
        case DemodMode::SW:
        case DemodMode::LW:
            softSSBDemod.setEnabled(false);
            _radio.setAM(b.freqMin, b.freqMax, dialKHz, b.stepKHz);
            _radio.setBandwidth(b.amBwIdx, 1);
            _radio.setAmSoftMuteMaxAttenuation(0);
            _radio.setAutomaticGainControl(1, 0);
            break;

        case DemodMode::LSB:
        case DemodMode::USB:
        case DemodMode::CW: {
            // Chip stays in AM mode; product detector does the SSB work
            uint32_t chipKHz = _chipFreqForDial(dialKHz);
            _radio.setAM(b.freqMin + b.lsbTuneOffsetKHz,
                         b.freqMax + b.lsbTuneOffsetKHz,
                         chipKHz, 1);
            _radio.setBandwidth(b.amBwIdx, 1);  // 3kHz for SSB, 1kHz for CW
            _radio.setAmSoftMuteMaxAttenuation(0);
            _radio.setAutomaticGainControl(0, 0); // AGC off for SSB

            int bfo = b.defaultBfoHz + _bfoTrimHz;
            softSSBDemod.setMode(mode, bfo);
            softSSBDemod.setEnabled(true);

            ESP_LOGI(TAG, "Soft SSB: chip=%u kHz  BFO=%d Hz  mode=%s",
                     chipKHz, bfo, demodModeStr(mode));
            break;
        }

        default:
            break;
    }

    _radio.setVolume(_currentVolume);
}

// ============================================================
// Volume
// ============================================================
void RadioController::setVolume(uint8_t vol) {
    _currentVolume = constrain(vol, 0, 63);
    _radio.setVolume(_currentVolume);
    lockStatus();
    _status.volume = _currentVolume;
    unlockStatus();
}

void RadioController::volumeUp()   { setVolume(min((int)_currentVolume + 2, 63)); }
void RadioController::volumeDown() { setVolume(max((int)_currentVolume - 2, 0));  }

void RadioController::mute(bool m) {
    if (m) _radio.setVolume(0);
    else   _radio.setVolume(_currentVolume);
}

// ============================================================
// setBFOTrim()
// Fine-tunes received pitch by ±500 Hz around the band default.
// ============================================================
void RadioController::setBFOTrim(int trimHz) {
    _bfoTrimHz = constrain(trimHz, -500, 500);
    if (isSoftSSBMode(_currentMode)) {
        _applySSBSoftDemod();
        const Band& b = BAND_TABLE[_currentBandIndex];
        lockStatus();
        _status.bfoHz = b.defaultBfoHz + _bfoTrimHz + _ssbFineTuneHz;
        unlockStatus();
    }
}

// ============================================================
// setBandwidth()
// ============================================================
void RadioController::setBandwidth(uint8_t amBwIdx) {
    _radio.setBandwidth(amBwIdx, 1);
}

// ============================================================
// setAGC()
// ============================================================
void RadioController::setAGC(bool enable, uint8_t gain) {
    _radio.setAutomaticGainControl(enable ? 0 : 1, gain);
    lockStatus();
    _status.agcEnabled = enable;
    _status.agcGain    = gain;
    unlockStatus();
}

// ============================================================
// _updateRSSI() — with I2C watchdog
// ============================================================
void RadioController::_updateRSSI() {
    _radio.getCurrentReceivedSignalQuality();
    uint8_t rssi = _radio.getCurrentRSSI();
    uint8_t snr  = _radio.getCurrentSNR();

    if (rssi == 0 && snr == 0) {
        if (++_consecutiveErrors >= I2C_RESET_THRESHOLD) {
            ESP_LOGW(TAG, "I2C zero-read threshold hit — resetting bus");
            _resetI2CBus();
            _consecutiveErrors = 0;
        }
    } else {
        _consecutiveErrors = 0;
    }

    // Read _currentMode under lock — it is written on Core 0 by _applyMode()
    lockStatus();
    bool isFM = (_currentMode == DemodMode::FM);
    unlockStatus();
    bool stereo = isFM ? _radio.getCurrentPilot() : false;
    lockStatus();
    _status.rssi      = rssi;
    _status.snr       = snr;
    _status.stereo    = stereo;
    _status.i2cErrors = _i2cErrors;
    unlockStatus();
}

// ============================================================
// _updateRDS()
// ============================================================
void RadioController::_updateRDS() {
    _radio.getRdsStatus();
    if (!_radio.getRdsReceived() || !_radio.getRdsSyncFound()) return;
    char* ps = _radio.getRdsText0A();
    char* rt = _radio.getRdsText2A();
    lockStatus();
    if (ps) strncpy(_status.rdsStationName, ps, 8);
    if (rt) strncpy(_status.rdsProgramInfo,  rt, 64);
    unlockStatus();
}

// ============================================================
// _updateBattery()
// Delegates to PowerManager which reads BQ27220 (accurate) or
// falls back to the BQ25896 ADC. The raw IO04 ADC approach has
// been removed — it was an unreliable voltage-divider estimate.
// ============================================================
void RadioController::_updateBattery() {
    const PowerStatus& ps = powerManager.getStatus();
    lockStatus();
    _status.batteryVolts   = ps.batteryVolts;
    _status.batteryPercent = ps.batteryPercent;
    _status.isCharging     = ps.isCharging;
    _status.isUsbConnected = ps.isUsbConnected;
    unlockStatus();
}

// ============================================================
// _resetI2CBus()
// ============================================================
void RadioController::_resetI2CBus() {
    _i2cErrors++;
    Wire.end();
    delay(50);
    Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);
    Wire.setTimeOut(100);
    delay(50);
    _radio.setVolume(_currentVolume);
    ESP_LOGI(TAG, "I2C reset #%lu complete", (unsigned long)_i2cErrors);
}
