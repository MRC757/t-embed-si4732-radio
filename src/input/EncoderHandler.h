#pragma once
#include <Arduino.h>
class EncoderHandler {
public:
    void begin();
    static void inputTask(void* arg);
private:
    void _inputLoop();
    void _handleEncoderDelta(int delta);
    void _handleSingleClick();
    void _handleLongPress();
    void _handleDoubleClick();
};
extern EncoderHandler encoderHandler;
