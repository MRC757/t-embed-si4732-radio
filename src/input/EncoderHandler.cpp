// ============================================================
// EncoderHandler.cpp — Rotary encoder + push button
// Short press: cycle through control targets (freq/volume/BFO/band)
// Long press (>800ms): toggle mute / change step
// Double press: seek
// Encoder rotation: change current target value
// ============================================================
#include "EncoderHandler.h"
#include "../radio/RadioController.h"
#include "../config/PinConfig.h"
#include <ESP32Encoder.h>
#include <esp_log.h>

static const char* TAG = "Encoder";
extern RadioController radioController;

static ESP32Encoder encoder;

// Control target
enum class Target { FREQUENCY, VOLUME, BFO, BAND };
static Target currentTarget = Target::FREQUENCY;

// Button state
static bool      btnLastState      = HIGH;
static uint32_t  btnPressMs        = 0;
static uint32_t  lastClickMs       = 0;
static int       clickCount        = 0;
static bool      longPressHandled  = false;
static bool      pendingSingleClick = false;

static constexpr uint32_t LONG_PRESS_MS   = 800;
static constexpr uint32_t DOUBLE_CLICK_MS = 400;

void EncoderHandler::begin() {
    ESP32Encoder::useInternalWeakPullResistors = puType::UP;
    encoder.attachHalfQuad(PIN_ENC_A, PIN_ENC_B);
    encoder.setCount(0);

    pinMode(PIN_ENC_BTN, INPUT_PULLUP);

    xTaskCreatePinnedToCore(
        inputTask, "Encoder", STACK_INPUT, this, TASK_PRIO_INPUT, nullptr, CORE_WEB
    );
    ESP_LOGI(TAG, "Encoder ready");
}

void EncoderHandler::inputTask(void* arg) {
    EncoderHandler* self = (EncoderHandler*) arg;
    self->_inputLoop();
}

void EncoderHandler::_inputLoop() {
    int64_t lastCount = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));

        // --- Encoder rotation ---
        int64_t count = encoder.getCount();
        int64_t delta = count - lastCount;
        if (delta != 0) {
            lastCount = count;
            _handleEncoderDelta((int)delta);
        }

        // --- Button ---
        bool btnNow = (digitalRead(PIN_ENC_BTN) == LOW); // active low
        uint32_t now = millis();

        if (btnNow && !btnLastState) {
            // Pressed
            btnPressMs = now;
            longPressHandled = false;
        }

        if (btnNow && btnLastState) {
            // Held
            if (!longPressHandled && (now - btnPressMs >= LONG_PRESS_MS)) {
                longPressHandled = true;
                pendingSingleClick = false; // cancel any deferred tap
                _handleLongPress();
            }
        }

        if (!btnNow && btnLastState) {
            // Released
            if (!longPressHandled) {
                uint32_t elapsed = now - lastClickMs;
                if (elapsed < DOUBLE_CLICK_MS) {
                    clickCount++;
                } else {
                    clickCount = 1;
                }
                lastClickMs = now;

                if (clickCount >= 2) {
                    clickCount = 0;
                    pendingSingleClick = false;
                    _handleDoubleClick();
                } else {
                    // Arm deferred single-click — dispatched below if no
                    // second click arrives within DOUBLE_CLICK_MS.
                    pendingSingleClick = true;
                }
            }
        }

        // Dispatch deferred single-click once the double-click window expires.
        if (pendingSingleClick && (now - lastClickMs >= DOUBLE_CLICK_MS)) {
            pendingSingleClick = false;
            clickCount = 0;
            _handleSingleClick();
        }

        btnLastState = btnNow;
    }
}

void EncoderHandler::_handleEncoderDelta(int delta) {
    // Normalise: encoder half-quad gives 2 counts per detent
    int steps = delta / 2;
    if (steps == 0) return;

    switch (currentTarget) {
        case Target::FREQUENCY:
            for (int i = 0; i < abs(steps); i++) {
                if (steps > 0) radioController.stepUp();
                else           radioController.stepDown();
            }
            break;
        case Target::VOLUME:
            for (int i = 0; i < abs(steps); i++) {
                if (steps > 0) radioController.volumeUp();
                else           radioController.volumeDown();
            }
            break;
        case Target::BFO: {
            // BFO target now trims ±500 Hz around the band default
            // Each encoder click = 10 Hz trim
            static int trimAccum = 0;
            trimAccum += steps * 10;
            trimAccum = constrain(trimAccum, -500, 500);
            radioController.setBFOTrim(trimAccum);
            break;
        }
        case Target::BAND:
            if (steps > 0) radioController.nextBand();
            else           radioController.prevBand();
            break;
    }
}

void EncoderHandler::_handleSingleClick() {
    // Cycle through control targets
    int next = ((int)currentTarget + 1) % 4;
    currentTarget = (Target)next;
    const char* names[] = {"FREQUENCY", "VOLUME", "BFO", "BAND"};
    ESP_LOGI(TAG, "Target → %s", names[next]);
}

void EncoderHandler::_handleLongPress() {
    if (currentTarget == Target::FREQUENCY) {
        // Seek
        ESP_LOGI(TAG, "Seek up");
        radioController.seekUp();
    } else if (currentTarget == Target::VOLUME) {
        // Mute toggle
        static bool muted = false;
        muted = !muted;
        radioController.mute(muted);
        ESP_LOGI(TAG, "Mute: %s", muted ? "ON" : "OFF");
    }
}

void EncoderHandler::_handleDoubleClick() {
    // Double click: reset BFO to zero
    if (currentTarget == Target::BFO) {
        radioController.setBFOTrim(0);
        ESP_LOGI(TAG, "BFO trim reset to 0");
    }
}

EncoderHandler encoderHandler;
