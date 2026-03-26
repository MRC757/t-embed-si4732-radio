#pragma once
// ============================================================
// WebSocketHandler.h
// Manages two WebSocket endpoints:
//
//   ws://device/ws/audio
//     Binary frames: [4B uint32 timestamp ms][512 × int16 PCM @ 12kHz]
//     Each frame = 512 samples = 1024 bytes PCM = 42.7ms of audio
//     Browser uses Web Audio API AudioWorklet to play in real-time.
//
//   ws://device/ws/radio
//     Mixed binary/text frames:
//       Binary: waterfall row (4-byte timestamp + 256 × float32 = 1028 bytes)
//       Text:   JSON status updates (RSSI, SNR, frequency, RDS, etc.)
// ============================================================
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "../config/PinConfig.h"

// Waterfall binary frame header magic number
static constexpr uint32_t WF_MAGIC = 0x57465246; // "WFRF"

// Audio frame size in samples (32ms @ 16kHz)
static constexpr size_t AUDIO_FRAME_SAMPLES = 512;
static constexpr size_t AUDIO_FRAME_BYTES   = AUDIO_FRAME_SAMPLES * sizeof(int16_t);

class WebSocketHandler {
public:
    WebSocketHandler();

    // Call from WebServer::begin() to register endpoints
    void attachToServer(AsyncWebServer& server);

    // Called periodically from the web stream task
    void broadcastAudioFrame();
    void broadcastWaterfallRow();
    void broadcastStatus();

    // Client count
    int audioClientCount() const;
    int radioClientCount() const;

    // Called from AsyncWebServer event loop — do not block
    void onAudioWsEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client,
                        AwsEventType type, void* arg, uint8_t* data, size_t len);
    void onRadioWsEvent(AsyncWebSocket* ws, AsyncWebSocketClient* client,
                        AwsEventType type, void* arg, uint8_t* data, size_t len);

    // FreeRTOS streaming task
    static void streamTask(void* arg);

private:
    AsyncWebSocket* _wsAudio;
    AsyncWebSocket* _wsRadio;

    uint32_t _lastStatusBroadcastMs;
    static constexpr uint32_t STATUS_BROADCAST_MS = 500;

    void _handleRadioCommand(const String& json);
    // includeBands=true on initial connect, false for periodic 500ms updates
    void _buildStatusJson(JsonDocument& doc, bool includeBands = false);
    void _broadcastMemList();   // send mem_list JSON to all radio clients
    void _streamLoop();
};

extern WebSocketHandler wsHandler;
