#pragma once
#include <Arduino.h>
#include "../radio/RadioController.h"

class DisplayManager {
public:
    void begin();
    void update();

    // Call from any input handler to wake the display and reset the idle timer.
    void wakeDisplay();

private:
    static constexpr uint32_t DIM_TIMEOUT_MS   = 30000;  // 30 s → dim
    static constexpr uint32_t SLEEP_TIMEOUT_MS = 120000; // 2 min → backlight off

    uint32_t _lastActivityMs = 0;
    uint8_t  _backlightLevel = 128;

    void _drawFrequency(const RadioStatus& s);
    void _drawModeAndBand(const RadioStatus& s);
    void _drawSignalMeter(const RadioStatus& s);
    void _drawRDS(const RadioStatus& s);
    void _drawBattery(const RadioStatus& s);
    void _drawSSBInfo(const RadioStatus& s);
};
extern DisplayManager displayManager;
