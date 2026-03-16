#pragma once
#include <Arduino.h>
#include "../radio/RadioController.h"

class DisplayManager {
public:
    void begin();
    void update();
private:
    void _drawFrequency(const RadioStatus& s);
    void _drawModeAndBand(const RadioStatus& s);
    void _drawSignalMeter(const RadioStatus& s);
    void _drawRDS(const RadioStatus& s);
    void _drawBattery(const RadioStatus& s);
    void _drawSSBInfo(const RadioStatus& s);
};
extern DisplayManager displayManager;
