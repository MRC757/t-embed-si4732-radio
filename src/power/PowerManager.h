#pragma once
// ============================================================
// PowerManager.h — Battery charging and fuel gauge
//
// Wraps two Texas Instruments ICs present on the T-Embed SI4732:
//
//   BQ25896  (I2C 0x6B) — USB-C battery charger.
//     Without explicit initialisation this chip does NOT enable
//     charging from USB-C. The T-Embed CC1101 issue tracker
//     confirmed that calling PPM.init() + PPM.enableCharge() is
//     required. We use lewisxhe/XPowersLib for the driver.
//
//   BQ27220  (I2C 0x55) — Battery fuel gauge.
//     Provides accurate state-of-charge (%), voltage, current,
//     and cycle count directly over I2C, replacing the crude
//     ADC voltage-divider estimate we used previously.
//
// THREAD SAFETY
// -------------
// begin() must be called from setup() on Core 0 after Wire is
// initialised. update() is called from loop() on Core 0 and
// is not thread-safe — status fields are updated atomically
// as simple assignments (all reads are from Core 0 too).
//
// GRACEFUL DEGRADATION
// --------------------
// If either chip does not respond (wrong pins, hardware fault),
// begin() logs a warning and the relevant fields return safe
// defaults. The rest of the system continues normally.
// ============================================================
#include <Arduino.h>
#define XPOWERS_CHIP_BQ25896   // select BQ25896 driver → enables XPowersPPM typedef
#include <XPowersLib.h>

struct PowerStatus {
    // From BQ27220 fuel gauge (accurate)
    int     batteryPercent;   // 0–100 %  (-1 if fuel gauge absent)
    float   batteryVolts;     // e.g. 3.85 V
    int     batteryCurrentMA; // charge=positive, discharge=negative mA
    int     batteryCycles;    // charge cycle count

    // From BQ25896 charger
    bool    isCharging;       // USB power actively charging battery
    bool    isUsbConnected;   // VBUS present (USB plugged in)
    float   vbusVolts;        // USB input voltage
    float   systemVolts;      // System rail voltage

    // Chip presence flags (set in begin(), never change)
    bool    chargerPresent;   // BQ25896 found on I2C
    bool    gaugePresent;     // BQ27220 found on I2C
};

class PowerManager {
public:
    PowerManager();

    // Initialise both chips. Call once from setup() after Wire.begin().
    // Returns true if at least one chip was found.
    bool begin();

    // Poll both chips. Call from loop() every ~5 seconds.
    void update();

    const PowerStatus& getStatus() const { return _status; }

    // Convenience accessors
    int   getBatteryPercent() const { return _status.batteryPercent; }
    float getBatteryVolts()   const { return _status.batteryVolts; }
    bool  isCharging()        const { return _status.isCharging; }
    bool  isUsbConnected()    const { return _status.isUsbConnected; }

    // LiPo voltage → SOC% via 13-point discharge curve (public for DisplayManager)
    static int voltageToPercent(float v);

private:
    XPowersPPM  _charger;   // BQ25896 via XPowersLib
    PowerStatus _status;

    bool _chargerOK;
    bool _gaugeOK;

    uint32_t _lastUpdateMs;
    static constexpr uint32_t UPDATE_INTERVAL_MS = 5000;

    bool _initCharger();
    bool _initGauge();
    void _updateCharger();
    void _updateGauge();
    static int _voltageToPercent(float v) { return voltageToPercent(v); }

    // BQ27220 raw I2C reads (XPowersLib doesn't include this gauge)
    // We use direct Wire calls with the standard BATTERY_STATUS register set.
    int16_t  _gaugeReadWord(uint8_t reg);
    uint16_t _gaugeReadUWord(uint8_t reg);

    static constexpr uint8_t BQ27220_ADDR         = 0x55;
    // BQ27220 standard command registers
    static constexpr uint8_t BQ27220_REG_VOLT      = 0x08; // mV
    static constexpr uint8_t BQ27220_REG_CURRENT   = 0x0C; // mA signed
    static constexpr uint8_t BQ27220_REG_SOC        = 0x2C; // %
    static constexpr uint8_t BQ27220_REG_CYCLES     = 0x2A; // count
};

extern PowerManager powerManager;
