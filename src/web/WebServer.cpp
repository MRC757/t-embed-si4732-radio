// ============================================================
// WebServer.cpp — HTTP server, Wi-Fi, REST API, LittleFS
//
// Wi-Fi strategy:
//   1. Always start AP (AP_SSID / AP_PASS from build flags).
//      The device is reachable at 192.168.4.1 at all times.
//   2. Load STA credentials from NVS (saved by the captive portal).
//      If NVS is empty, fall back to compile-time WIFI_SSID/WIFI_PASS.
//   3. If credentials exist, try STA connect (15 s timeout) in
//      WIFI_AP_STA mode so the AP stays up while connecting.
//   4. On STA success: radio UI accessible on both STA and AP IPs.
//   5. On STA failure or no credentials: captive portal activates.
//      DNS server redirects all queries to the AP IP.
//      /wifi serves an embedded Wi-Fi setup page (scan + credentials).
//      /wifi/save stores credentials in NVS and reboots.
// ============================================================
#include "WebServer.h"
#include "WebSocketHandler.h"
#include "../radio/RadioController.h"
#include "../radio/BandConfig.h"
#include "../audio/AudioCapture.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_log.h>

static const char* TAG = "WebServer";

extern RadioController radioController;

static AsyncWebServer httpServer(80);
static DNSServer      dnsServer;
static bool           _captivePortalActive = false;
static bool           _staConnected        = false;
static String         _apIP;

// ============================================================
// Captive portal — Wi-Fi setup page (embedded, no LittleFS needed)
// ============================================================
static const char WIFI_SETUP_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html lang="en"><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-Embed Radio — Wi-Fi Setup</title>
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;max-width:460px;margin:32px auto;padding:16px;background:#111;color:#eee}
  h2{color:#4af;margin:0 0 4px}
  p.sub{color:#888;font-size:.9em;margin:0 0 16px}
  .net{display:flex;justify-content:space-between;align-items:center;
       padding:10px 14px;margin:5px 0;background:#222;border-radius:6px;
       cursor:pointer;border:1px solid transparent}
  .net:hover,.net.sel{background:#1a3a5c;border-color:#4af}
  .rssi{font-size:.8em;color:#888}
  label{display:block;margin:14px 0 4px;color:#aaa;font-size:.9em}
  input{width:100%;padding:10px;background:#222;color:#eee;
        border:1px solid #444;border-radius:6px;font-size:1em}
  .btn{display:block;width:100%;padding:12px;margin-top:18px;
       border:none;border-radius:6px;font-size:1.05em;font-weight:bold;cursor:pointer}
  #scan-btn{background:#2a2a2a;color:#4af}
  #save-btn{background:#4af;color:#000}
  #save-btn:disabled{background:#444;color:#777;cursor:not-allowed}
  #status{margin-top:12px;text-align:center;min-height:18px;color:#4af;font-size:.9em}
  #nets-wrap{min-height:40px}
  .hint{color:#666;font-size:.85em;padding:8px 0}
</style></head><body>
<h2>Wi-Fi Setup</h2>
<p class="sub">Select a network or type the name, enter the password, then tap Connect.</p>
<button id="scan-btn" class="btn" onclick="doScan()">&#8635; Scan for networks</button>
<div id="nets-wrap"><p class="hint">Scanning&hellip;</p></div>
<label for="ssid">Network name (SSID)</label>
<input id="ssid" type="text" placeholder="Select above or type manually">
<label for="pass">Password</label>
<input id="pass" type="password" placeholder="Leave blank for open networks">
<button id="save-btn" class="btn" disabled onclick="doSave()">Connect &amp; Save</button>
<div id="status"></div>
<script>
var retryTimer=null;
function bar(r){return r>-60?'&#9602;&#9604;&#9606;&#9608;':r>-70?'&#9602;&#9604;&#9606;':r>-80?'&#9602;&#9604;':'&#9602;';}
function status(msg){document.getElementById('status').innerHTML=msg;}
function doScan(){
  clearTimeout(retryTimer);
  var w=document.getElementById('nets-wrap');
  w.innerHTML='<p class="hint">Scanning&hellip;</p>';
  document.getElementById('scan-btn').disabled=true;
  fetch('/wifi/scan').then(function(r){return r.json();}).then(function(nets){
    w.innerHTML='';
    if(!nets||nets.length===0){
      w.innerHTML='<p class="hint">No networks found &mdash; retrying&hellip;</p>';
      retryTimer=setTimeout(doScan,3000);
      document.getElementById('scan-btn').disabled=false;
      return;
    }
    nets.forEach(function(n){
      var el=document.createElement('div');
      el.className='net';
      el.innerHTML='<span>'+n.ssid+'</span><span class="rssi">'+bar(n.rssi)+' '+(n.open?'open':'&#128274;')+'</span>';
      el.onclick=function(){
        document.querySelectorAll('.net').forEach(function(x){x.classList.remove('sel');});
        el.classList.add('sel');
        document.getElementById('ssid').value=n.ssid;
        document.getElementById('save-btn').disabled=false;
        document.getElementById('pass').focus();
      };
      w.appendChild(el);
    });
    document.getElementById('scan-btn').disabled=false;
  }).catch(function(){
    w.innerHTML='<p class="hint" style="color:#f66">Scan failed &mdash; retrying&hellip;</p>';
    retryTimer=setTimeout(doScan,3000);
    document.getElementById('scan-btn').disabled=false;
  });
}
document.getElementById('ssid').addEventListener('input',function(){
  document.getElementById('save-btn').disabled=this.value.trim()==='';
});
function doSave(){
  var ssid=document.getElementById('ssid').value.trim();
  var pass=document.getElementById('pass').value;
  if(!ssid){status('Please enter a network name.');return;}
  document.getElementById('save-btn').disabled=true;
  status('Saving&hellip; device will reboot and connect.');
  fetch('/wifi/save',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:ssid,pass:pass})})
  .then(function(r){return r.json();})
  .then(function(d){status(d.msg||'Rebooting&hellip;');})
  .catch(function(){status('Saved. Rebooting&hellip;');});
}
doScan();
</script></body></html>
)rawhtml";

// ============================================================
// NVS credential helpers
// ============================================================
static Preferences wifiPrefs;

struct WifiCreds { String ssid; String pass; };

static WifiCreds loadCreds() {
    wifiPrefs.begin("wifi", true);
    WifiCreds c;
    c.ssid = wifiPrefs.getString("ssid", "");
    c.pass = wifiPrefs.getString("pass", "");
    wifiPrefs.end();
    return c;
}

static void saveCreds(const String& ssid, const String& pass) {
    wifiPrefs.begin("wifi", false);
    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("pass", pass);
    wifiPrefs.end();
}

// ============================================================
// Background Wi-Fi scan task
// Populates _scanResultsJson; _scanInProgress guards re-entry.
// ============================================================
static String _scanResultsJson = "[]";
static bool   _scanInProgress  = false;

static void scanTask(void*) {
    // Hide AP's own SSID from results; include hidden=false
    int n = WiFi.scanNetworks(false, false);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; i++) {
        // Skip our own AP SSID
        if (WiFi.SSID(i) == AP_SSID) continue;
        JsonObject net = arr.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    }
    serializeJson(doc, _scanResultsJson);
    _scanInProgress = false;
    vTaskDelete(NULL);
}

static void triggerScan() {
    if (_scanInProgress) return;
    _scanInProgress = true;
    _scanResultsJson = "[]"; // clear stale results
    xTaskCreatePinnedToCore(scanTask, "WiFiScan", 4096, nullptr,
                            1, nullptr, CORE_WEB);
}

// ============================================================
// Captive portal redirect helpers
// ============================================================
static void redirectToPortal(AsyncWebServerRequest* req) {
    req->redirect("http://" + _apIP + "/wifi");
}

// ============================================================
// Wi-Fi init — AP+STA simultaneous, captive portal fallback
// ============================================================
static void wifiBegin() {
    // AP always starts first so device is reachable immediately
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    _apIP = WiFi.softAPIP().toString();
    ESP_LOGI(TAG, "AP started: SSID=%s  IP=%s", AP_SSID, _apIP.c_str());

    // Load credentials — NVS first, then compile-time fallback
    WifiCreds creds = loadCreds();
    if (creds.ssid.isEmpty()) {
        const String ctSsid = WIFI_SSID;
        if (!ctSsid.isEmpty() && ctSsid != "your_wifi_ssid") {
            creds.ssid = ctSsid;
            creds.pass = WIFI_PASS;
            ESP_LOGI(TAG, "No NVS credentials — using compile-time defaults");
        }
    } else {
        ESP_LOGI(TAG, "Loaded NVS credentials for \"%s\"", creds.ssid.c_str());
    }

    if (creds.ssid.isEmpty()) {
        ESP_LOGW(TAG, "No credentials — captive portal active");
        _captivePortalActive = true;
        dnsServer.start(53, "*", WiFi.softAPIP());
        triggerScan(); // pre-populate scan while user navigates to portal
        return;
    }

    ESP_LOGI(TAG, "Attempting STA connect to \"%s\"", creds.ssid.c_str());
    WiFi.begin(creds.ssid.c_str(), creds.pass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 15000) {
            ESP_LOGW(TAG, "STA connect timed out — captive portal active on AP");
            _captivePortalActive = true;
            dnsServer.start(53, "*", WiFi.softAPIP());
            triggerScan();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    _staConnected = true;
    ESP_LOGI(TAG, "STA connected. STA IP=%s  AP IP=%s",
             WiFi.localIP().toString().c_str(), _apIP.c_str());
}

// ============================================================
// REST API handlers
// ============================================================

static void handleGetStatus(AsyncWebServerRequest* req) {
    radioController.lockStatus();
    const RadioStatus& s = radioController.getStatus();

    JsonDocument doc;
    doc["freq"]      = s.dialKHz;
    doc["freqHz"]    = s.displayFreqHz;
    doc["chipKHz"]   = s.frequencyKHz;
    doc["bandIndex"] = s.bandIndex;
    doc["mode"]      = demodModeStr(s.mode);
    doc["band"]      = BAND_TABLE[s.bandIndex].name;
    doc["rssi"]      = s.rssi;
    doc["snr"]       = s.snr;
    doc["stereo"]    = s.stereo;
    doc["volume"]    = s.volume;
    doc["agc"]       = s.agcEnabled;
    doc["ssb"]       = true;
    doc["bat"]       = s.batteryVolts;
    doc["batPct"]    = s.batteryPercent;
    doc["charging"]  = s.isCharging;
    doc["rds"]       = s.rdsStationName;
    doc["staIP"]     = _staConnected ? WiFi.localIP().toString() : "";
    doc["apIP"]      = _apIP;

    radioController.unlockStatus();

    String body;
    serializeJson(doc, body);
    req->send(200, "application/json", body);
}

static void handleTune(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"bad JSON\"}"); return;
    }
    uint32_t freq = doc["freq"] | 0;
    if (freq == 0) { req->send(400, "application/json", "{\"error\":\"missing freq\"}"); return; }
    radioController.setFrequency(freq);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void handleMode(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad JSON\"}"); return; }
    const char* modeStr = doc["mode"] | "";
    DemodMode mode = DemodMode::FM;
    if      (strcmp(modeStr, "AM")  == 0) mode = DemodMode::AM;
    else if (strcmp(modeStr, "LSB") == 0) mode = DemodMode::LSB;
    else if (strcmp(modeStr, "USB") == 0) mode = DemodMode::USB;
    else if (strcmp(modeStr, "CW")  == 0) mode = DemodMode::CW;
    else if (strcmp(modeStr, "LW")  == 0) mode = DemodMode::LW;
    else if (strcmp(modeStr, "SW")  == 0) mode = DemodMode::SW;
    radioController.setMode(mode);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void handleBand(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"error\":\"bad JSON\"}"); return; }
    int idx = doc["index"] | -1;
    if (idx < 0 || idx >= BAND_COUNT) { req->send(400, "application/json", "{\"error\":\"invalid band\"}"); return; }
    radioController.setBand(idx);
    req->send(200, "application/json", "{\"ok\":true}");
}

static void handleGetBands(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["bands"].to<JsonArray>();
    for (int i = 0; i < BAND_COUNT; i++) {
        JsonObject b = arr.add<JsonObject>();
        b["index"]   = i;
        b["name"]    = BAND_TABLE[i].name;
        b["mode"]    = demodModeStr(BAND_TABLE[i].mode);
        b["min"]     = BAND_TABLE[i].freqMin;
        b["max"]     = BAND_TABLE[i].freqMax;
        b["default"] = BAND_TABLE[i].freqDefault;
        b["ssb"]     = isSoftSSBMode(BAND_TABLE[i].mode);
    }
    String body;
    serializeJson(doc, body);
    req->send(200, "application/json", body);
}

static void handleGetFT8Freqs(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < FT8_FREQ_COUNT; i++) {
        JsonObject f = arr.add<JsonObject>();
        f["band"] = FT8_FREQS[i].band;
        f["freq"] = FT8_FREQS[i].freqKHz;
    }
    String body;
    serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// ============================================================
// Captive portal handlers
// ============================================================

// GET /wifi — setup page
static void handleWifiPage(AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", WIFI_SETUP_HTML);
}

// GET /wifi/scan — return cached scan JSON, trigger fresh scan
static void handleWifiScan(AsyncWebServerRequest* req) {
    triggerScan(); // kick off a new scan (no-op if one is running)
    req->send(200, "application/json", _scanResultsJson);
}

// POST /wifi/save  body: {"ssid":"MyNet","pass":"secret"}
static void handleWifiSave(AsyncWebServerRequest* req, uint8_t* data,
                            size_t len, size_t, size_t) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"bad JSON\"}");
        return;
    }
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["pass"] | "";
    if (!ssid || strlen(ssid) == 0) {
        req->send(400, "application/json", "{\"error\":\"ssid required\"}");
        return;
    }
    saveCreds(ssid, pass);
    ESP_LOGI(TAG, "Credentials saved for \"%s\" — rebooting", ssid);
    req->send(200, "application/json",
              "{\"ok\":true,\"msg\":\"Saved! Connecting in 3 seconds...\"}");
    // Reboot after response is sent
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

// Standard captive-portal detection endpoints — redirect to setup page.
// iOS, Android, Windows, and macOS each probe different URLs.
static void registerCaptiveDetectRoutes() {
    // Android
    httpServer.on("/generate_204", HTTP_GET, redirectToPortal);
    httpServer.on("/gen_204",      HTTP_GET, redirectToPortal);
    // iOS / macOS
    httpServer.on("/hotspot-detect.html",      HTTP_GET, redirectToPortal);
    httpServer.on("/library/test/success.html",HTTP_GET, redirectToPortal);
    httpServer.on("/success.txt",              HTTP_GET, redirectToPortal);
    // Windows
    httpServer.on("/connecttest.txt",          HTTP_GET, redirectToPortal);
    httpServer.on("/redirect",                 HTTP_GET, redirectToPortal);
    httpServer.on("/ncsi.txt",                 HTTP_GET, redirectToPortal);
}

// ============================================================
// webServerBegin() — call from setup()
// ============================================================
void webServerBegin() {
    if (!LittleFS.begin(false)) {
        ESP_LOGE(TAG, "LittleFS mount failed — web UI unavailable");
        ESP_LOGE(TAG, "Run: pio run --target uploadfs");
    } else {
        ESP_LOGI(TAG, "LittleFS mounted. Free: %lu bytes",
                 LittleFS.totalBytes() - LittleFS.usedBytes());
    }

    wifiBegin();

    wsHandler.attachToServer(httpServer);

    // ── Captive portal routes (registered regardless; only reachable via AP) ──
    httpServer.on("/wifi",       HTTP_GET,  handleWifiPage);
    httpServer.on("/wifi/scan",  HTTP_GET,  handleWifiScan);
    httpServer.on("/wifi/save",  HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr, handleWifiSave);
    if (_captivePortalActive) {
        registerCaptiveDetectRoutes();
    }

    // ── REST API ──
    httpServer.on("/api/status",   HTTP_GET,  handleGetStatus);
    httpServer.on("/api/bands",    HTTP_GET,  handleGetBands);
    httpServer.on("/api/ft8freqs", HTTP_GET,  handleGetFT8Freqs);
    httpServer.on("/api/tune",     HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handleTune);
    httpServer.on("/api/mode",     HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handleMode);
    httpServer.on("/api/band",     HTTP_POST,
        [](AsyncWebServerRequest* r){}, nullptr, handleBand);

    // ── Static files ──
    httpServer.serveStatic("/", LittleFS, "/")
              .setDefaultFile("index.html")
              .setCacheControl("max-age=600");

    // ── 404 — captive portal redirect or SPA fallback ──
    httpServer.onNotFound([](AsyncWebServerRequest* req) {
        if (_captivePortalActive) {
            redirectToPortal(req);
        } else {
            req->send(LittleFS, "/index.html", "text/html");
        }
    });

    httpServer.begin();
    ESP_LOGI(TAG, "HTTP server running on port 80");
    if (_staConnected) {
        ESP_LOGI(TAG, "Radio UI: http://%s  (also http://%s)",
                 WiFi.localIP().toString().c_str(), _apIP.c_str());
    } else {
        ESP_LOGI(TAG, "Captive portal: connect to \"%s\" then open http://%s/wifi",
                 AP_SSID, _apIP.c_str());
    }
}

// ============================================================
// webLoop() — call from loop() on Core 0
// Polls the DNS server when the captive portal is active.
// ============================================================
void webLoop() {
    if (_captivePortalActive) {
        dnsServer.processNextRequest();
    }
}
