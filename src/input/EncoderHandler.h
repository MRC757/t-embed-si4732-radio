#pragma once
#include <Arduino.h>
class EncoderHandler {
public:
    void begin();
    static void inputTask(void* arg);
    int  getTargetIndex() const;   // 0=FREQ 1=VOL 2=BFO 3=BAND
private:
    void _inputLoop();
    void _handleEncoderDelta(int delta);
    void _handleSingleClick();
    void _handleLongPress();
    void _handleDoubleClick();
    static void _enterDeepSleep();  // called after 10-second button hold
};
extern EncoderHandler encoderHandler;
