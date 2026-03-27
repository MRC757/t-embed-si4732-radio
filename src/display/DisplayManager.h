#pragma once
#include <Arduino.h>
#include "../radio/RadioController.h"

class DisplayManager {
public:
    void begin();
    void update();

    // Call from any input handler to wake the display and reset the idle timer.
    void wakeDisplay();

    // Sleep countdown overlay.
    // secsRemaining: 1–9 = show countdown, 0 = show "Sleeping..." then clear,
    //               -1 = cancelled (hide overlay).
    void setSleepCountdown(int secsRemaining);

    // Render the final "Going to sleep" frame and blank the backlight.
    // Blocks ~600 ms — call immediately before esp_deep_sleep_start().
    void drawSleepScreen();

private:
    static constexpr uint32_t DIM_TIMEOUT_MS   = 60000;  // 60 s → dim
    static constexpr uint32_t SLEEP_TIMEOUT_MS = 300000; // 5 min → backlight off

    uint32_t _lastActivityMs  = 0;
    uint8_t  _backlightLevel  = 128;
    int      _sleepCountdown  = -1;  // -1 = inactive

    void _drawFrequency(const RadioStatus& s);
    void _drawModeAndBand(const RadioStatus& s);
    void _drawSignalMeter(const RadioStatus& s);
    void _drawRDS(const RadioStatus& s);
    void _drawBattery(const RadioStatus& s);
    void _drawSSBInfo(const RadioStatus& s);
    void _drawWifiStatus();
    void _drawEncoderTarget();
    void _drawClock();
    void _drawSleepCountdownOverlay();
};
extern DisplayManager displayManager;
