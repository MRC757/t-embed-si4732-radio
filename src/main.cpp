// ============================================================
// main.cpp — LILYGO T-Embed SI4732 Web Radio Receiver
//
// Initialisation order (matters — do not reorder):
//   1.  Serial
//   2.  APA102 LED (visual boot progress)
//   3.  IO46 HIGH — powers SI4732 module and its I2C pull-ups
//   4.  I2C scanner — auto-detects SDA/SCL pin order,
//         leaves Wire configured on the winning pins
//   5.  PowerManager (BQ25896 charger + BQ27220 fuel gauge)
//         Must run BEFORE radio so charging is enabled immediately
//   6.  RadioController (SI4732 via I2C)
//   7.  AudioCapture (ADC on IO17 — Option 1 passive mute bias test)
//   8.  FFTProcessor (waterfall DSP)
//   9.  EncoderHandler (rotary input)
//   10. WebServer (Wi-Fi + HTTP + WebSocket)
//   11. FreeRTOS tasks
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
#include <esp_system.h>

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

    // Log why we rebooted — critical for diagnosing unexpected resets
    esp_reset_reason_t resetReason = esp_reset_reason();
    const char* resetStr =
        resetReason == ESP_RST_POWERON   ? "POWER_ON"    :
        resetReason == ESP_RST_EXT       ? "EXT_PIN"     :
        resetReason == ESP_RST_SW        ? "SOFTWARE"    :
        resetReason == ESP_RST_PANIC     ? "PANIC/CRASH" :
        resetReason == ESP_RST_INT_WDT   ? "INT_WDT"     :
        resetReason == ESP_RST_TASK_WDT  ? "TASK_WDT"    :
        resetReason == ESP_RST_WDT       ? "OTHER_WDT"   :
        resetReason == ESP_RST_DEEPSLEEP ? "DEEP_SLEEP"  :
        resetReason == ESP_RST_BROWNOUT  ? "BROWNOUT"    : "UNKNOWN";
    Serial.printf("[init] Reset reason: %s (%d)\n", resetStr, (int)resetReason);

    // --------------------------------------------------------
    // 1. APA102 status LED — red during boot
    //
    // Drive CLK low before FastLED init. GPIO45 (APA102_CLK) has a
    // default pull-down from the schematic, but making it explicit
    // prevents any glitch on the LED chain during power-on.
    // --------------------------------------------------------
    pinMode(PIN_LED_CLK, OUTPUT);
    digitalWrite(PIN_LED_CLK, LOW);

    FastLED.addLeds<APA102, PIN_LED_DATA, PIN_LED_CLK, BGR>(leds, NUM_LEDS);
    FastLED.setBrightness(32);
    setLED(CRGB::Red);

    // --------------------------------------------------------
    // 2. SI4732 module power enable — MUST precede I2C scan.
    //
    // The SI4732 module plugs into a connector on the T-Embed.
    // The I2C pull-up resistors are on the module PCB and are
    // only powered when IO46 is HIGH.  Without this the bus
    // floats and both SDA/SCL pin orders find 0 devices.
    //
    // RadioController::begin() also asserts IO46, but that runs
    // at step 5 — long after the I2C scanner at step 3.
    // --------------------------------------------------------
    pinMode(PIN_SI4732_PWR, OUTPUT);
    digitalWrite(PIN_SI4732_PWR, HIGH);
    delay(100);   // allow module VCC + pull-ups to settle
    Serial.println("[init] IO46 HIGH — SI4732 module powered");

    // --------------------------------------------------------
    // 3. I2C auto-detection
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
    // 5. Power management — BQ25896 + BQ27220
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
    // 6. Display
    // --------------------------------------------------------
    displayManager.begin();
    Serial.println("[init] Display ready");

    // --------------------------------------------------------
    // 7. Radio (SI4732, software SSB, no patch required)
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
    // 8. Speaker unmute + Option 3 audio capture.
    //
    // IO17 driven LOW to unmute the analog speaker amp.
    // AudioCapture uses I2S_NUM_1 as slave RX on IO07/05/06
    // to receive SI4732 digital audio output (48 kHz stereo).
    // --------------------------------------------------------
    pinMode(PIN_AUDIO_MUTE, OUTPUT);
    digitalWrite(PIN_AUDIO_MUTE, LOW);
    Serial.println("[init] Speaker unmuted (IO17 LOW)");

    if (!audioCapture.begin()) {
        Serial.println("[WARN] Audio capture failed — web stream disabled");
    } else {
        Serial.println("[init] I2S slave RX ready (IO06/05/07 @ 48kHz)");
    }

    // --------------------------------------------------------
    // 9. FFT waterfall
    // --------------------------------------------------------
    if (!fftProcessor.begin()) {
        Serial.println("[WARN] FFT failed — waterfall disabled");
    } else {
        Serial.println("[init] FFT ready");
    }

    // --------------------------------------------------------
    // 10. Encoder
    // --------------------------------------------------------
    encoderHandler.begin();
    Serial.println("[init] Encoder ready");

    // --------------------------------------------------------
    // 11. Wi-Fi + web server
    // --------------------------------------------------------
    setLED(CRGB::Blue);
    webServerBegin();
    Serial.println("[init] Web server ready");

    // --------------------------------------------------------
    // 12. FreeRTOS tasks
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
                      s.displayFreqHz / 1000000.0f,
                      s.rssi, s.snr,
                      s.batteryPercent,
                      s.batteryVolts,
                      s.isCharging ? "(CHG)" : "",
                      ESP.getFreeHeap(),
                      ESP.getFreePsram(),
                      (unsigned long)audioCapture.droppedSamples());
        radioController.unlockStatus();
        // Stack high-water marks help diagnose overflow-related resets
        Serial.printf("[diag] Stack HWM: loop=%u\n",
                      uxTaskGetStackHighWaterMark(NULL));
    }

    vTaskDelay(pdMS_TO_TICKS(5));
}
