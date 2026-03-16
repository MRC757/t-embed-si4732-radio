// ============================================================
// PowerManager.cpp
// ============================================================
#include "PowerManager.h"
#include "../config/PinConfig.h"
#include <Wire.h>
#include <esp_log.h>

static const char* TAG = "PowerMgr";

PowerManager::PowerManager()
    : _chargerOK(false)
    , _gaugeOK(false)
    , _lastUpdateMs(0)
{
    memset(&_status, 0, sizeof(_status));
    _status.batteryPercent   = -1;   // -1 = gauge not yet read
    _status.batteryVolts     = 0.0f;
    _status.chargerPresent   = false;
    _status.gaugePresent     = false;
}

// ============================================================
// begin()
// ============================================================
bool PowerManager::begin() {
    ESP_LOGI(TAG, "Initialising power management...");

    _chargerOK = _initCharger();
    _gaugeOK   = _initGauge();

    _status.chargerPresent = _chargerOK;
    _status.gaugePresent   = _gaugeOK;

    if (!_chargerOK && !_gaugeOK) {
        ESP_LOGW(TAG, "Neither BQ25896 nor BQ27220 found.");
        ESP_LOGW(TAG, "Battery will not charge from USB until BQ25896 is initialised.");
        ESP_LOGW(TAG, "Verify I2C SDA/SCL pins using I2CScanner output above.");
        return false;
    }

    // Do first read immediately so status is populated at boot
    if (_chargerOK) _updateCharger();
    if (_gaugeOK)   _updateGauge();

    ESP_LOGI(TAG, "Power OK — charger:%s gauge:%s  Bat=%.2fV %d%% %s",
             _chargerOK ? "yes" : "NO",
             _gaugeOK   ? "yes" : "NO",
             _status.batteryVolts,
             _status.batteryPercent,
             _status.isCharging ? "CHARGING" : "discharging");
    return true;
}

// ============================================================
// _initCharger() — BQ25896 via XPowersLib
// ============================================================
bool PowerManager::_initCharger() {
    // XPowersLib PPM driver for BQ25896
    // Wire, sda, scl, address — uses the already-initialised Wire instance
    bool ok = _charger.init(Wire, I2C_SDA, I2C_SCL, BQ25896_SLAVE_ADDRESS);
    if (!ok) {
        ESP_LOGW(TAG, "BQ25896 (0x6B) not found — USB charging disabled");
        return false;
    }

    // ── Charger configuration ─────────────────────────────────
    // Values taken from the working T-Embed CC1101 community example
    // (https://github.com/Xinyuan-LilyGO/T-Embed-CC1101/issues/9)
    _charger.setSysPowerDownVoltage(3300);   // system cutoff 3.3V
    _charger.setInputCurrentLimit(3250);     // 3.25A max USB-C input
    _charger.disableCurrentLimitPin();       // ignore ILIM hardware pin
    _charger.setChargeTargetVoltage(4208);   // 4.208V charge target (LiPo max)
    _charger.setPrechargeCurr(64);           // 64mA pre-charge (safe for 900mAh)
    _charger.setChargerConstantCurr(832);    // 832mA constant charge (~0.9C)
    _charger.enableMeasure();                // enable voltage/current ADC
    _charger.enableCharge();                 // START CHARGING — critical call

    ESP_LOGI(TAG, "BQ25896 configured. Charging enabled. "
                  "Target=%.0fmV ConstCurr=%dmA",
             4208.0f,
             (int)_charger.getChargerConstantCurr());
    return true;
}

// ============================================================
// _initGauge() — BQ27220 via direct I2C
// XPowersLib does not include BQ27220 support; we use the
// standard GAUGE command register interface directly.
// ============================================================
bool PowerManager::_initGauge() {
    // Probe: attempt to read the voltage register
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(BQ27220_REG_VOLT);
    uint8_t err = Wire.endTransmission(false);
    if (err != 0) {
        ESP_LOGW(TAG, "BQ27220 (0x55) not found — using charger ADC for voltage");
        return false;
    }
    ESP_LOGI(TAG, "BQ27220 fuel gauge found at 0x55");
    return true;
}

// ============================================================
// update() — call from loop() every ~5 seconds
// ============================================================
void PowerManager::update() {
    uint32_t now = millis();
    if (now - _lastUpdateMs < UPDATE_INTERVAL_MS) return;
    _lastUpdateMs = now;

    if (_chargerOK) _updateCharger();
    if (_gaugeOK)   _updateGauge();
}

// ============================================================
// voltageToPercent() — LiPo discharge curve lookup (single cell)
// Points derived from a typical 18650/LiPo constant-current discharge profile.
// Linear interpolation between break-points; clamps to [0, 100].
// ============================================================
int PowerManager::voltageToPercent(float v) {
    static const struct { float v; int pct; } lut[] = {
        { 4.20f, 100 }, { 4.10f,  90 }, { 4.00f,  80 },
        { 3.90f,  70 }, { 3.80f,  60 }, { 3.70f,  50 },
        { 3.60f,  40 }, { 3.50f,  30 }, { 3.40f,  20 },
        { 3.30f,  10 }, { 3.20f,   5 }, { 3.10f,   2 },
        { 3.00f,   0 },
    };
    static constexpr int LUT_LEN = sizeof(lut) / sizeof(lut[0]);

    if (v >= lut[0].v)           return 100;
    if (v <= lut[LUT_LEN - 1].v) return 0;

    for (int i = 0; i < LUT_LEN - 1; i++) {
        if (v >= lut[i + 1].v) {
            float frac = (v - lut[i + 1].v) / (lut[i].v - lut[i + 1].v);
            return lut[i + 1].pct + (int)(frac * (lut[i].pct - lut[i + 1].pct));
        }
    }
    return 0;
}

// ============================================================
// _updateCharger()
// ============================================================
void PowerManager::_updateCharger() {
    _status.isCharging    = _charger.isCharging();
    _status.isUsbConnected = _charger.isVbusIn();
    _status.vbusVolts     = _charger.getVbusVoltage()   / 1000.0f;
    _status.systemVolts   = _charger.getSystemVoltage() / 1000.0f;

    // If fuel gauge absent, fall back to charger ADC for voltage estimate
    if (!_gaugeOK) {
        _status.batteryVolts   = _charger.getBattVoltage() / 1000.0f;
        _status.batteryPercent = _voltageToPercent(_status.batteryVolts);
    }
}

// ============================================================
// _updateGauge() — BQ27220 register reads
// ============================================================
void PowerManager::_updateGauge() {
    int16_t  current = _gaugeReadWord(BQ27220_REG_CURRENT);
    uint16_t voltage = _gaugeReadUWord(BQ27220_REG_VOLT);
    uint16_t soc     = _gaugeReadUWord(BQ27220_REG_SOC);
    uint16_t cycles  = _gaugeReadUWord(BQ27220_REG_CYCLES);

    // Sanity check: voltage should be 2.5–4.4V for a LiPo
    if (voltage < 2500 || voltage > 4400) {
        ESP_LOGW(TAG, "BQ27220 voltage reading out of range (%u mV) — skipping", voltage);
        return;
    }

    _status.batteryVolts     = voltage / 1000.0f;
    _status.batteryPercent   = constrain((int)soc, 0, 100);
    _status.batteryCurrentMA = (int)current;
    _status.batteryCycles    = (int)cycles;

    // BQ27220 current: positive = charging, negative = discharging
    // This is the ground truth source for isCharging when gauge is present
    _status.isCharging = (current > 10);  // >10mA = charging
}

// ============================================================
// BQ27220 raw I2C helpers
// Standard two-byte register read (little-endian)
// ============================================================
int16_t PowerManager::_gaugeReadWord(uint8_t reg) {
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    Wire.requestFrom((uint8_t)BQ27220_ADDR, (uint8_t)2);
    if (Wire.available() < 2) return 0;
    uint8_t lo = Wire.read();
    uint8_t hi = Wire.read();
    return (int16_t)((hi << 8) | lo);
}

uint16_t PowerManager::_gaugeReadUWord(uint8_t reg) {
    return (uint16_t)_gaugeReadWord(reg);
}

// Global singleton
PowerManager powerManager;
