// ============================================================
// main.cpp — LILYGO T-Embed SI4732 Web Radio Receiver
//
// Initialisation order (matters — do not reorder):
//   1.  Serial
//   2.  APA102 LED (visual boot progress)
//   3.  I2C scanner — auto-detects SDA/SCL pin order,
//         leaves Wire configured on the winning pins
//   4.  PowerManager (BQ25896 charger + BQ27220 fuel gauge)
//         Must run BEFORE radio so charging is enabled immediately
//   5.  RadioController (SI4732 via I2C)
//   6.  AudioCapture (ADC DMA on IO17)
//   7.  FFTProcessor (waterfall DSP)
//   8.  EncoderHandler (rotary input)
//   9.  WebServer (Wi-Fi + HTTP + WebSocket)
//   10. FreeRTOS tasks
//
// Task layout:
//   Core 1 (radio/audio):
//     AudioADC  — ADC DMA fill, SoftSSBDemod processing
//     RadioCtrl — SI4732 RSSI/RDS polling, I2C watchdog
//     FFTDsp    — Hann FFT → waterfall rows
//
//   Core 0 (network/UI):
//     WSStream  — WebSocket audio + waterfall broadcast
//     Encoder   — rotary encoder debounce
//     loop()    — display 10fps, PowerManager poll, diagnostics
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <FastLED.h>
#include <esp_task_wdt.h>

#include "config/PinConfig.h"
#include "power/I2CScanner.h"
#include "power/PowerManager.h"
#include "radio/RadioController.h"
#include "radio/BandConfig.h"
#include "audio/AudioCapture.h"
#include "dsp/FFTProcessor.h"
#include "web/WebServer.h"
#include "web/WebSocketHandler.h"
#include "display/DisplayManager.h"
#include "input/EncoderHandler.h"

// ============================================================
// Global instances — extern'd by all modules
// ============================================================
RadioController radioController;

// ============================================================
// APA102 RGB LED
// ============================================================
static CRGB leds[NUM_LEDS];

static void setLED(CRGB colour) {
    leds[0] = colour;
    FastLED.show();
}

// ============================================================
// Radio task — Core 1
// Registered with task watchdog. update() calls wdt_reset().
// Hard I2C hang >5s triggers clean reboot.
// ============================================================
static void radioTask(void* /*arg*/) {
    esp_task_wdt_add(NULL);
    for (;;) {
        radioController.update();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ============================================================
// setup()
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n\n=== T-Embed SI4732 Web Radio ===");
    Serial.printf("IDF: %s\n", esp_get_idf_version());

    // --------------------------------------------------------
    // 1. APA102 status LED — red during boot
    // --------------------------------------------------------
    FastLED.addLeds<APA102, PIN_LED_DATA, PIN_LED_CLK, BGR>(leds, NUM_LEDS);
    FastLED.setBrightness(32);
    setLED(CRGB::Red);

    // --------------------------------------------------------
    // 2. I2C auto-detection
    //
    // Tries SDA/SCL in both possible orders, reports all found
    // devices, leaves Wire on the winning configuration.
    //
    // Expected on working hardware (4 devices):
    //   0x40  ES7210 microphone ADC
    //   0x55  BQ27220 battery fuel gauge
    //   0x63  SI4732 radio tuner
    //   0x6B  BQ25896 battery charger
    //
    // Once confirmed, lock I2C_SDA/I2C_SCL in PinConfig.h.
    // --------------------------------------------------------
    Serial.println("[init] I2C scanner...");
    auto scanResult = I2CScanner::scanAndReport();
    Serial.printf("[init] I2C: SDA=%d SCL=%d  %d device(s)\n",
                  scanResult.sda, scanResult.scl, scanResult.devicesFound);

    // --------------------------------------------------------
    // 3. Power management — BQ25896 + BQ27220
    //
    // CRITICAL: Without this step the battery will NOT charge
    // from USB-C. The BQ25896 requires explicit enableCharge()
    // to be called after every power cycle.
    // --------------------------------------------------------
    setLED(CRGB::Yellow);
    bool powerOK = powerManager.begin();
    if (!powerOK) {
        Serial.println("[WARN] Power init failed — USB charging disabled.");
        Serial.println("[WARN] Check I2C pins using scanner output above.");
    } else {
        Serial.printf("[init] Power OK — %d%%  %s\n",
                      powerManager.getBatteryPercent(),
                      powerManager.isCharging() ? "CHARGING" : "on battery");
    }

    // --------------------------------------------------------
    // 4. Display
    // --------------------------------------------------------
    displayManager.begin();
    Serial.println("[init] Display ready");

    // --------------------------------------------------------
    // 5. Radio (SI4732, software SSB, no patch required)
    // --------------------------------------------------------
    setLED(CRGB::Orange);
    if (!radioController.begin()) {
        Serial.println("[ERROR] SI4732 init failed — restarting in 3s");
        Serial.println("[ERROR] Check IO46 power enable and I2C pins");
        setLED(CRGB::Red);
        Serial.flush();
        delay(3000);
        esp_restart();
    }
    Serial.println("[init] SI4732 ready");

    // --------------------------------------------------------
    // 6. Audio capture — ADC DMA on IO17
    // --------------------------------------------------------
    if (!audioCapture.begin()) {
        Serial.println("[WARN] Audio capture failed — streaming disabled");
    } else {
        Serial.println("[init] Audio capture ready (IO17, 16kHz)");
    }

    // --------------------------------------------------------
    // 7. FFT waterfall
    // --------------------------------------------------------
    if (!fftProcessor.begin()) {
        Serial.println("[WARN] FFT failed — waterfall disabled");
    } else {
        Serial.println("[init] FFT ready");
    }

    // --------------------------------------------------------
    // 8. Encoder
    // --------------------------------------------------------
    encoderHandler.begin();
    Serial.println("[init] Encoder ready");

    // --------------------------------------------------------
    // 9. Wi-Fi + web server
    // --------------------------------------------------------
    setLED(CRGB::Blue);
    webServerBegin();
    Serial.println("[init] Web server ready");

    // --------------------------------------------------------
    // 10. FreeRTOS tasks
    // --------------------------------------------------------
    static TaskHandle_t radioTaskHandle = nullptr;
    xTaskCreatePinnedToCore(
        radioTask, "RadioCtrl",
        STACK_RADIO, nullptr,
        TASK_PRIO_RADIO, &radioTaskHandle,
        CORE_RADIO
    );

    // --------------------------------------------------------
    // Done
    // --------------------------------------------------------
    setLED(CRGB::Green);
    Serial.println("[init] Boot complete.");
    Serial.printf("[init] Heap=%u  PSRAM=%u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());

    // Pin confirmation summary — easy to find in boot log
    Serial.println("------------------------------------");
    Serial.printf("I2C active: SDA=%d  SCL=%d\n", scanResult.sda, scanResult.scl);
    Serial.printf("SI4732:  %s\n", scanResult.foundSI4732  ? "found" : "NOT FOUND");
    Serial.printf("BQ25896: %s\n", scanResult.foundBQ25896 ? "found" : "NOT FOUND — charging disabled");
    Serial.printf("BQ27220: %s\n", scanResult.foundBQ27220 ? "found" : "NOT FOUND — voltage estimate only");
    Serial.printf("ES7210:  %s\n", scanResult.foundES7210  ? "found" : "NOT FOUND");
    Serial.println("------------------------------------");
}

// ============================================================
// loop() — Core 0
// ============================================================
static uint32_t _lastDisplayMs   = 0;
static uint32_t _lastHeartbeatMs = 0;
static uint32_t _lastDiagMs      = 0;
static bool     _ledPhase        = false;

void loop() {
    uint32_t now = millis();

    // Display at ~10 fps
    if (now - _lastDisplayMs >= 100) {
        _lastDisplayMs = now;
        displayManager.update();
    }

    // PowerManager polls BQ27220/BQ25896 every 5s internally
    powerManager.update();

    // DNS server for captive portal (no-op when portal is not active)
    webLoop();

    // LED heartbeat — slow green blink = healthy
    if (now - _lastHeartbeatMs >= 1000) {
        _lastHeartbeatMs = now;
        _ledPhase = !_ledPhase;
        setLED(_ledPhase ? CRGB(0, 20, 0) : CRGB::Black);
    }

    // Serial diagnostics every 30 seconds
    if (now - _lastDiagMs >= 30000) {
        _lastDiagMs = now;
        radioController.lockStatus();
        const RadioStatus& s = radioController.getStatus();
        Serial.printf("[diag] %s %.3fMHz RSSI=%d SNR=%d "
                      "Bat=%d%% %.2fV%s "
                      "Heap=%u PSRAM=%u Drop=%lu\n",
                      demodModeStr(s.mode),
                      s.dialKHz / 1000.0f,
                      s.rssi, s.snr,
                      s.batteryPercent,
                      s.batteryVolts,
                      s.isCharging ? "(CHG)" : "",
                      ESP.getFreeHeap(),
                      ESP.getFreePsram(),
                      (unsigned long)audioCapture.droppedSamples());
        radioController.unlockStatus();
    }

    vTaskDelay(pdMS_TO_TICKS(5));
}
