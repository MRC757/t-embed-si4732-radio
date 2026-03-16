// ============================================================
// WebSocketHandler.cpp
// ============================================================
#include "WebSocketHandler.h"
#include "../audio/AudioCapture.h"
#include "../dsp/FFTProcessor.h"
#include "../radio/RadioController.h"
#include "../radio/BandConfig.h"
#include "../web/WebServer.h"
#include <esp_log.h>
#include <time.h>

static const char* TAG = "WSHandler";

// Forward declarations of global instances (defined in main.cpp)
extern RadioController radioController;

WebSocketHandler::WebSocketHandler()
    : _wsAudio(nullptr)
    , _wsRadio(nullptr)
    , _lastStatusBroadcastMs(0)
{}

// ============================================================
// attachToServer()
// ============================================================
void WebSocketHandler::attachToServer(AsyncWebServer& server) {
    _wsAudio = new AsyncWebSocket("/ws/audio");
    _wsRadio = new AsyncWebSocket("/ws/radio");

    _wsAudio->onEvent([this](AsyncWebSocket* ws, AsyncWebSocketClient* client,
                              AwsEventType type, void* arg, uint8_t* data, size_t len) {
        onAudioWsEvent(ws, client, type, arg, data, len);
    });

    _wsRadio->onEvent([this](AsyncWebSocket* ws, AsyncWebSocketClient* client,
                              AwsEventType type, void* arg, uint8_t* data, size_t len) {
        onRadioWsEvent(ws, client, type, arg, data, len);
    });

    server.addHandler(_wsAudio);
    server.addHandler(_wsRadio);

    // Start the streaming task pinned to Core 0 (web core)
    xTaskCreatePinnedToCore(
        streamTask, "WSStream", STACK_WEBSTREAM, this, TASK_PRIO_WEBSTREAM, nullptr, CORE_WEB
    );

    ESP_LOGI(TAG, "WebSocket endpoints: /ws/audio  /ws/radio");
}

// ============================================================
// Event handlers
// ============================================================
void WebSocketHandler::onAudioWsEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client,
                                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        ESP_LOGI(TAG, "Audio client #%u connected from %s",
                 client->id(), client->remoteIP().toString().c_str());
        // Send initial status on connect
        JsonDocument doc;
        _buildStatusJson(doc);
        String msg;
        serializeJson(doc, msg);
        client->text(msg);
    } else if (type == WS_EVT_DISCONNECT) {
        ESP_LOGI(TAG, "Audio client #%u disconnected", client->id());
    } else if (type == WS_EVT_ERROR) {
        ESP_LOGW(TAG, "Audio WS error #%u", client->id());
    }
    // Audio stream is one-directional (server → client), ignore incoming data
}

void WebSocketHandler::onRadioWsEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client,
                                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        ESP_LOGI(TAG, "Radio client #%u connected", client->id());
        // Send full status immediately on connect
        JsonDocument doc;
        _buildStatusJson(doc);
        String msg;
        serializeJson(doc, msg);
        client->text(msg);
    } else if (type == WS_EVT_DISCONNECT) {
        ESP_LOGI(TAG, "Radio client #%u disconnected", client->id());
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (info->opcode == WS_TEXT && len > 0) {
            String cmd = String((char*)data).substring(0, len);
            _handleRadioCommand(cmd);
        }
    }
}

// ============================================================
// _handleRadioCommand() — process JSON command from browser
// ============================================================
void WebSocketHandler::_handleRadioCommand(const String& json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        ESP_LOGW(TAG, "Bad JSON: %s", err.c_str());
        return;
    }

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "tune") == 0) {
        uint32_t freq = doc["freq"] | 0;
        if (freq > 0) radioController.setFrequency(freq);

    } else if (strcmp(cmd, "mode") == 0) {
        const char* modeStr = doc["mode"] | "";
        DemodMode mode = DemodMode::FM;
        if      (strcmp(modeStr, "FM")   == 0) mode = DemodMode::FM;
        else if (strcmp(modeStr, "AM")   == 0) mode = DemodMode::AM;
        else if (strcmp(modeStr, "LSB")  == 0) mode = DemodMode::LSB;
        else if (strcmp(modeStr, "USB")  == 0) mode = DemodMode::USB;
        else if (strcmp(modeStr, "CW")   == 0) mode = DemodMode::CW;
        else if (strcmp(modeStr, "LW")   == 0) mode = DemodMode::LW;
        else if (strcmp(modeStr, "SW")   == 0) mode = DemodMode::SW;
        radioController.setMode(mode);

    } else if (strcmp(cmd, "band") == 0) {
        int idx = doc["index"] | -1;
        if (idx >= 0) radioController.setBand(idx);

    } else if (strcmp(cmd, "volume") == 0) {
        uint8_t vol = doc["value"] | 40;
        radioController.setVolume(vol);

    } else if (strcmp(cmd, "bfo") == 0) {
        // "bfo" command sends a trim value ±500 Hz around the band default.
        // The full BFO = band.defaultBfoHz + trim (+ fine-tune from encoder).
        int trimHz = doc["hz"] | 0;
        radioController.setBFOTrim(trimHz);

    } else if (strcmp(cmd, "agc") == 0) {
        bool en  = doc["enable"] | true;
        uint8_t g = doc["gain"] | 0;
        radioController.setAGC(en, g);

    } else if (strcmp(cmd, "seek_up") == 0) {
        radioController.seekUp();

    } else if (strcmp(cmd, "seek_down") == 0) {
        radioController.seekDown();

    } else if (strcmp(cmd, "step_up") == 0) {
        radioController.stepUp();

    } else if (strcmp(cmd, "step_down") == 0) {
        radioController.stepDown();

    } else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd);
    }
}

// ============================================================
// broadcastAudioFrame() — send 512 PCM samples to all audio clients
// ============================================================
void WebSocketHandler::broadcastAudioFrame() {
    if (audioClientCount() == 0) return;

    static int16_t pcmBuf[AUDIO_FRAME_SAMPLES];
    size_t got = audioCapture.read(pcmBuf, AUDIO_FRAME_SAMPLES, pdMS_TO_TICKS(5));
    if (got == 0) return;

    // Binary frame (all fields little-endian — ESP32-S3 native byte order):
    //   [4 bytes] uint32 timestamp ms  → DataView.getUint32(0, true)
    //   [N × 2 bytes] int16 PCM        → new Int16Array(buf.buffer, 4)
    static uint8_t frameBuf[8 + AUDIO_FRAME_BYTES];
    uint32_t ts = millis();
    memcpy(frameBuf,     &ts,    4);
    memcpy(frameBuf + 4, pcmBuf, got * sizeof(int16_t));

    _wsAudio->binaryAll(frameBuf, 4 + got * sizeof(int16_t));
}

// ============================================================
// broadcastWaterfallRow() — send FFT magnitudes to radio clients
// ============================================================
void WebSocketHandler::broadcastWaterfallRow() {
    if (radioClientCount() == 0) return;

    WaterfallRow row;
    if (!fftProcessor.getWaterfallRow(row)) return;

    // Binary frame (all fields little-endian — ESP32-S3 native byte order):
    //   [4 bytes] uint32 magic = 0x57465246 ("WFRF") → DataView.getUint32(0, true)
    //   [4 bytes] uint32 timestamp ms                → DataView.getUint32(4, true)
    //   [N × 4 bytes] float32 magnitude bins [0, 1]  → new Float32Array(buf.buffer, 8)
    static uint8_t wfBuf[8 + WATERFALL_COLS * 4];
    uint32_t ts = row.timestampMs;
    memcpy(wfBuf,     &WF_MAGIC, 4);
    memcpy(wfBuf + 4, &ts,       4);
    memcpy(wfBuf + 8, row.bins,  WATERFALL_COLS * sizeof(float));

    _wsRadio->binaryAll(wfBuf, sizeof(wfBuf));
}

// ============================================================
// broadcastStatus() — send JSON radio status to all radio clients
// ============================================================
void WebSocketHandler::broadcastStatus() {
    if (radioClientCount() == 0) return;
    uint32_t now = millis();
    if (now - _lastStatusBroadcastMs < STATUS_BROADCAST_MS) return;
    _lastStatusBroadcastMs = now;

    JsonDocument doc;
    _buildStatusJson(doc);
    String msg;
    serializeJson(doc, msg);
    _wsRadio->textAll(msg);
}

// ============================================================
// _buildStatusJson()
// ============================================================
void WebSocketHandler::_buildStatusJson(JsonDocument& doc) {
    radioController.lockStatus();
    const RadioStatus& s = radioController.getStatus();

    doc["type"]        = "status";
    doc["freq"]        = s.dialKHz;        // user dial frequency kHz
    doc["freqHz"]      = s.displayFreqHz;  // display Hz (dial+finetune for SSB)
    doc["chipKHz"]     = s.frequencyKHz;   // SI4732 chip frequency kHz
    doc["bfoHz"]       = s.bfoHz;          // effective BFO frequency Hz
    doc["mode"]        = demodModeStr(s.mode);
    doc["bandIndex"]   = s.bandIndex;
    doc["band"]        = BAND_TABLE[s.bandIndex].name;
    doc["rssi"]        = s.rssi;
    doc["snr"]         = s.snr;
    doc["stereo"]      = s.stereo;
    doc["volume"]      = s.volume;
    doc["agc"]         = s.agcEnabled;
    doc["agcGain"]     = s.agcGain;
    doc["rdsName"]     = s.rdsStationName;
    doc["rdsProg"]     = s.rdsProgramInfo;
    doc["ssb"]         = true; // Software SSB always available
    doc["bat"]         = s.batteryVolts;
    doc["batPct"]      = s.batteryPercent;
    doc["charging"]    = s.isCharging;
    doc["usbIn"]       = s.isUsbConnected;
    doc["sampleRate"]  = AUDIO_SAMPLE_RATE_HZ;
    doc["dropped"]     = audioCapture.droppedSamples();
    doc["ts"]          = millis();

    radioController.unlockStatus();

    // UTC timestamp — lets the browser correct its own clock for FT8 slot alignment.
    // Only included when NTP has synchronised; browser falls back to Date.now() otherwise.
    bool ntpSynced = isNtpSynced();
    doc["ntpSynced"] = ntpSynced;
    if (ntpSynced) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        doc["utcMs"] = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }

    // Add band table on first connect (type="status" → browser checks for bandTable)
    JsonArray bands = doc["bands"].to<JsonArray>();
    for (int i = 0; i < BAND_COUNT; i++) {
        JsonObject b = bands.add<JsonObject>();
        b["index"]   = i;
        b["name"]    = BAND_TABLE[i].name;
        b["mode"]    = demodModeStr(BAND_TABLE[i].mode);
        b["default"] = BAND_TABLE[i].freqDefault;
        b["ssb"]     = isSoftSSBMode(BAND_TABLE[i].mode); // software SSB: no patch needed
    }
}

// ============================================================
// streamTask — continuous streaming loop
// ============================================================
void WebSocketHandler::streamTask(void* arg) {
    WebSocketHandler* self = (WebSocketHandler*) arg;
    self->_streamLoop();
}

void WebSocketHandler::_streamLoop() {
    const TickType_t period = pdMS_TO_TICKS(20); // 50 Hz loop
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&lastWakeTime, period);

        // Clean up disconnected clients periodically
        if (_wsAudio) _wsAudio->cleanupClients();
        if (_wsRadio) _wsRadio->cleanupClients();

        // Stream audio frames (~50 per second = 50 × 32ms chunks ahead of playback)
        broadcastAudioFrame();

        // Waterfall at ~10 fps (every 5th loop iteration)
        static uint8_t wfCounter = 0;
        if (++wfCounter >= 5) {
            wfCounter = 0;
            broadcastWaterfallRow();
        }

        // Status JSON at ~2 Hz
        broadcastStatus();
    }
}

int WebSocketHandler::audioClientCount() const {
    return _wsAudio ? _wsAudio->count() : 0;
}

int WebSocketHandler::radioClientCount() const {
    return _wsRadio ? _wsRadio->count() : 0;
}

WebSocketHandler wsHandler;
