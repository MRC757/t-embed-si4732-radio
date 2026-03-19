# LILYGO T-Embed SI4732 — Web-Enabled HAM Radio Receiver

A complete firmware for the **LILYGO T-Embed SI4732** that turns it into a
web-connected multiband radio receiver with real-time spectrum waterfall,
audio streaming, software SSB/CW demodulation, and browser-based FT8/JS8Call
decoding.

---

## Features

### Radio Reception
- **FM** — 64–108 MHz with RDS station name and programme text
- **AM (MW)** — 520–1710 kHz
- **Longwave (LW)** — 153–279 kHz
- **Shortwave (SW)** — 2.3–26.1 MHz across 13 broadcast bands
- **Citizens Band (CB)** — 26.965–27.405 MHz AM, 40 channels, ch 19 default
- **Marine HF** — 4 / 8 / 16 MHz USB voice bands with ITU calling frequencies
- **Time signals** — WWV/WWVH at 2.5 / 5 / 10 / 15 / 20 MHz (AM)
- **LSB / USB (SSB)** — all HAM bands 160m through 10m, software demodulated
- **CW** — software demodulated with 700 Hz BFO
- **36 pre-configured bands** including all major HAM, broadcast, CB, Marine HF, and WWV allocations

> **Out-of-range note:** GMRS (462 MHz), FRS (462–467 MHz), MURS (151–154 MHz),
> and VHF Air (118–136 MHz) are **above the SI4732's 30 MHz upper limit** and
> cannot be received with this hardware.

### SSB — No Patch Required
SSB and CW reception uses a **software product detector** (`SoftSSBDemod`)
running on the ESP32-S3. The SI4732 stays in AM mode; the ESP32 multiplies the
audio by a synthesized BFO carrier and low-pass filters the result. This
eliminates the fragile Silicon Labs binary patch entirely:

- No I2C patch download at boot
- No patch wipe on mode switch
- No reload after setFM()/setAM()
- Instant mode switching between AM, FM, SSB, CW

LSB and USB use different SI4732 chip tuning offsets so the sideband falls
correctly in the AM passband. See `BandConfig.h` for the tuning notes.

### Web Interface
- Live frequency display and tuning controls
- Real-time **spectrum waterfall** (FFT, 10 fps, 256 bins)
- **Audio streaming** — 12 kHz 16-bit PCM over WebSocket, Web Audio API playback
- **Browser-side BFO pitch trim** — instant pitch adjustment without
  a network round-trip, processed in the AudioWorklet
- **FT8 / FT4 decoder** — browser-side pure-TypeScript decoder (ft8ts), UTC
  slot-aligned, no PC software or WASM compilation needed
- **JS8Call decoder** — browser-side Rust WASM decoder; full LDPC + callsign
  decode; supports all four speed modes (Slow 30 s, Normal 15.6 s, Fast 10 s,
  Turbo 6 s) at 12 kHz; built from the included `js8call-decoder/` project
- **Browser-side CW decoder** — Goertzel filter locked to BFO pitch, adaptive
  envelope threshold, automatic WPM estimation (5–40 WPM), full ITU character
  set; tracks BFO slider in real time
- **NTP time sync** — ESP32 NTP client feeds accurate UTC to the browser;
  configurable server via web GUI; **Sync Now** button for manual alignment
  when internet NTP is unavailable
- Band and mode selector with all 36 bands
- **S-meter** — RSSI displayed as S1–S9 / S9+dB (S9 = 34 dBμV, 6 dB/unit)
- SNR bar and RDS display for FM stations
- Volume, AGC, BFO trim, and CW decoder controls

### Power Management
- **BQ25896** USB-C battery charger — initialised at boot so the 900 mAh
  LiPo actually charges when USB-C is connected
- **BQ27220** battery fuel gauge — accurate state-of-charge %, voltage,
  current, and cycle count over I2C
- Battery fallback (gauge absent): 13-point LiPo discharge curve lookup
  instead of a linear approximation
- Battery % and charging indicator on the local TFT display and web UI
- **Wi-Fi modem sleep** — enabled after STA connect; reduces WiFi TX current
  ~30% with negligible latency impact
- **WebSocket stream throttle** — stream task drops to 100 ms loop period
  when no browser clients are connected (vs 20 ms active), saving CPU

### Hardware UI
- 1.9" ST7789V IPS TFT — frequency, mode, band, S-meter, RDS, battery
- **Auto-dim / auto-sleep** — TFT backlight dims after 30 s idle, turns off
  after 2 min; any encoder or button input restores full brightness
- Rotary encoder — tunes frequency, volume, BFO trim, or band (press cycles)
- Long press — seek / mute toggle
- Double press — reset BFO trim to zero

### Connectivity
- **Wi-Fi AP+STA simultaneous** — AP (`T-Embed-Radio`) always running; also joins
  your network when credentials are saved
- **Captive portal** — on first boot (or STA failure), connecting to the AP opens
  an automatic setup page: scan nearby networks, select one, enter the password.
  Credentials are saved to NVS flash and survive reboots
- **I2C watchdog** — automatic bus recovery if SI4732 communication hangs

---

## Hardware

| Item | Detail |
|------|--------|
| MCU | ESP32-S3 Dual-core LX7 @ 240 MHz |
| Flash | 16 MB |
| PSRAM | 8 MB OPI |
| Radio IC | Skyworks SI4732-A10 |
| Display | 1.9" ST7789V IPS TFT, 170×320 px |
| Connectivity | Wi-Fi 802.11 b/g/n, BLE 5 |
| Audio input | SI4732 analog audio → IO17 (ADC DMA) |
| Audio output | I2S speaker amplifier (IO07/05/06) |
| Microphone | ES7210 I2S ADC (IO47/21/14/48) |
| LED | APA102 RGB (IO45/42) |
| Storage | MicroSD SPI (IO39/40/41/38) |
| Charger | BQ25896 (I2C 0x6B) |
| Fuel gauge | BQ27220 (I2C 0x55) |
| Power | USB-C or 3.7V LiPo 900 mAh |

### Pin Map

> All pins verified from the official LILYGO T-Embed SI4732 product pinmap image.

| Peripheral | Pins |
|------------|------|
| ST7789 LCD | BL=IO15, DC=IO13, CS=IO10, CLK=IO12, MOSI=IO11, RST=IO09 |
| Encoder | A=IO02, B=IO01, Button=IO00 (BOOT) |
| I2S Speaker | BCLK=IO07, WCLK=IO05, DOUT=IO06 |
| ES7210 Mic | BCLK=IO47, LRCK=IO21, DIN=IO14, MCLK=IO48 |
| APA102 LED | CLK=IO45, DATA=IO42 |
| MicroSD | CS=IO39, SCLK=IO40, MOSI=IO41, MISO=IO38 |
| SI4732 I2C | SDA=IO18, SCL=IO08 |
| SI4732 Audio | Analog → IO17 (ADC1_CH6) |
| SI4732 Power | IO46 (active HIGH) |
| BQ25896 / BQ27220 | Same I2C bus as SI4732 |

> I2C pin order confirmed from the official LILYGO schematic and pinmap photo
> (SDA=IO18, SCL=IO08). The boot-time `I2CScanner` still runs and will report
> if a hardware variant is encountered with the opposite order.

#### Full I2C Bus

All four devices share one bus (400 kHz):

| Device | Address | Purpose |
|--------|---------|---------|
| SI4732 | 0x63 (SEN=VCC) or 0x11 (SEN=GND) | Radio tuner |
| ES7210 | 0x40 | Microphone ADC |
| BQ27220 | 0x55 | Battery fuel gauge |
| BQ25896 | 0x6B | USB-C battery charger |

---

## ES7210 Audio Path Upgrade (Planned)

The ES7210 quad-channel I2S ADC is already on the board and connected to the
ESP32-S3. Only two of its four mic input channels (MIC1, MIC2) are used by the
onboard MEMS microphones. MIC3 and MIC4 inputs are routed to the ES7210 but
left unconnected on the PCB, making MIC3 an ideal high-quality input for the
SI4732 analog audio.

Replacing the ESP32 ADC path (IO17, ~9-10 ENOB) with the ES7210 (24-bit I2S)
would dramatically improve weak-signal FT8 and SSB reception.

### Hardware modification

| Point | Location | Notes |
|-------|----------|-------|
| **Source** | GPIO expansion header pin labeled **SCL** (bottom of board) | This is IO17 = SI4732 analog audio output, ~1.65 V DC + audio |
| **Destination** | ES7210 **MIC3P** (IC pin 31) | Requires soldering directly to the ES7210 QFN pad |
| **MIC3N** | ES7210 pin 32 | Tie to AGND (analog ground) |
| **MICBIAS34** | ES7210 pin 26 | Already has bypass caps C29/C30 — no work needed |
| **REFP34** | ES7210 pin 29 | Already has bypass cap — no work needed |

**Required component:** one **100 nF** ceramic capacitor in series on the signal
line for AC coupling (removes the 1.65 V DC offset from the SI4732 output
before it enters the ES7210 differential input).

```
SI4732 audio out
(Expansion header SCL / IO17)
        |
       [100nF]          ← AC coupling cap
        |
    ES7210 MIC3P (pin 31)
    ES7210 MIC3N (pin 32) ── AGND
```

### Firmware changes required

Once the hardware is wired, the firmware changes are:

1. **`PinConfig.h`** — add `PIN_ES7210_MIC3_CHANNEL 2` (ES7210 channel index for
   MIC3) and set `PIN_SI4732_AUDIO -1` to disable the ADC path.
2. **`AudioCapture.cpp`** — replace the `adc_digi_*` capture loop with an I2S
   read from `I2S_PORT_MIC` (already defined as `I2S_NUM_1`). The ES7210
   driver (`ES7210.h` from lewisxhe) needs to be configured for single-channel
   24-bit capture on channel 2 (MIC3).
3. **`AudioCapture.h`** — remove `AUDIO_ADC_CHANNEL`, `AUDIO_ADC_UNIT`,
   `AUDIO_ADC_ATTEN`, `AUDIO_ADC_BITWIDTH` constants. Add `ES7210_MIC_CHANNEL`.
4. **Sample rate** — the ES7210 supports 8 / 12 / 16 / 44.1 / 48 kHz. At 12 kHz
   it matches the stream rate and the ft8_lib decoder natively.

> Until this modification is made, the firmware continues to use the ESP32-S3
> ADC DMA path on IO17 with software calibration and AGC.

---

## Audio Architecture

The SI4732 on this board outputs **analog audio only** (no I2S pins exposed).

```
SI4732 analog audio (IO17)
       |
       v
ESP32-S3 ADC1_CH6 — continuous DMA, 12 kHz, 12-bit
       |
       v
esp_adc_cal_raw_to_voltage()  [corrects ±50 LSB INL bow]
       |
       v
IIR DC blocker  y[n] = x[n] - x[n-1] + 0.9999·y[n-1]  [removes DC offset]
       |
       +-- SoftSSBDemod.process()   [in-place, when SSB/CW active]
       |     DDS BFO oscillator (sin LUT, 32-bit phase accumulator)
       |     4th-order Butterworth LPF (two cascaded biquads)
       |
       v
_applyAGC()  [software AGC — RMS tracking, attack 0.3, release 0.02]
       |
       +-- PSRAM ring buffer (~5.5 s deep)
             |
             +-- WebSocket /ws/audio  --> browser Web Audio API
             |     AudioWorklet + optional browser BFO pitch trim
             |
             +-- FFTProcessor  --> waterfall bins --> /ws/radio
             |
             +-- ft8Decoder    --> 15 s slot buffer --> ft8ts decode (FT8/FT4)
             |
             +-- js8Decoder    --> Slow/Normal/Fast/Turbo slot buffer
             |                     --> js8call_wasm LDPC decode (callsigns)
             |
             +-- cwDecoder     --> Goertzel @ BFO Hz --> Morse lookup
                                   automatic WPM, full ITU table
```

---

## Software Architecture

### Source Tree
```
src/
├── config/
│   └── PinConfig.h            All GPIO assignments + I2C addresses
├── power/
│   ├── I2CScanner.h           Boot-time bus scanner (SDA/SCL auto-detect)
│   ├── PowerManager.h/.cpp    BQ25896 charger + BQ27220 fuel gauge
├── radio/
│   ├── RadioController.h/.cpp SI4732 via PU2CLR; AM/FM/SW/LW/SSB/CW
│   ├── BandConfig.h           36 bands, FT8 quick-tune table, SSB tuning notes
│   └── SSBPatch.h             Stub (patch approach replaced by SoftSSBDemod)
├── dsp/
│   ├── SoftSSBDemod.h/.cpp    Software product detector (no patch needed)
│   ├── FFTProcessor.h/.cpp    Hann-windowed FFT → waterfall magnitude bins
├── audio/
│   └── AudioCapture.h/.cpp    ADC DMA, ADC cal, IIR DC-blocker, ring buffer
├── web/
│   ├── WebServer.cpp          Wi-Fi AP+STA, captive portal, NTP, REST API
│   └── WebSocketHandler.h/.cpp /ws/audio PCM + /ws/radio waterfall+status
├── display/
│   └── DisplayManager.h/.cpp  Sprite TFT UI, S-meter, auto-dim/sleep
├── input/
│   └── EncoderHandler.h/.cpp  Rotary encoder, calls wakeDisplay()
└── main.cpp                   FreeRTOS tasks, I2CScanner, PowerManager init
data/                          LittleFS web UI (pio run -t uploadfs)
├── index.html
├── css/style.css
└── js/
    ├── app.js                 UI controller, S-meter, NTP status
    ├── audio.js               Web Audio API PCM player + browser BFO
    ├── waterfall.js           Canvas waterfall renderer (heat/ice/green)
    ├── ft8.js                 FT8/FT4 decoder, NTP slot alignment
    ├── ft8worker.js           Module Web Worker — imports ft8ts.mjs, energy fallback
    ├── ft8ts.mjs              ft8ts pure-TS FT8/FT4 decoder (download separately)
    ├── js8call.js             JS8Call decoder controller
    ├── js8call-worker.js      Module Web Worker — imports js8call_wasm.js
    ├── js8call_wasm.js        wasm-bindgen JS glue (build from js8call-decoder/)
    ├── js8call_wasm_bg.wasm   JS8Call WASM binary (build from js8call-decoder/)
    └── cw.js                  CW decoder — Goertzel, auto-WPM, Morse table
```

### FreeRTOS Tasks

| Task | Core | Priority | Purpose |
|------|------|----------|---------|
| AudioADC | 1 | 6 | ADC DMA → SoftSSBDemod → AGC → ring buffer |
| RadioCtrl | 1 | 5 | SI4732 RSSI/RDS poll, I2C watchdog |
| FFTDsp | 1 | 4 | FFT waterfall rows |
| Encoder | 0 | 4 | Rotary encoder debounce |
| WSStream | 0 | 3 | WebSocket audio + waterfall broadcast |
| loop() | 0 | — | Display 10 fps, PowerManager poll |

### WebSocket Protocol

**`ws://device/ws/audio`** — Binary, server→client, all fields little-endian
```
[4 bytes uint32 timestamp ms][N × int16 PCM @ 12 kHz]
Frame = 512 samples = ~42.7 ms audio

JS decode:
  const ts  = view.getUint32(0, true);
  const pcm = new Int16Array(buf.buffer, 4);
```

**`ws://device/ws/radio`** — Mixed, bidirectional
```
Binary (server→client):  waterfall row, all fields little-endian
  [4B uint32 magic 0x57465246 "WFRF"][4B uint32 timestamp][256 × float32 0–1]

JS decode:
  const magic = view.getUint32(0, true);
  const ts    = view.getUint32(4, true);
  const bins  = new Float32Array(buf.buffer, 8);

Text (server→client):  JSON status @ 2 Hz
  { "type":"status",
    "freq":14074,          // dial frequency kHz
    "freqHz":14074000,     // display frequency Hz (dial + BFO fine-tune for SSB)
    "chipKHz":14074,       // SI4732 chip tuning frequency kHz
    "bfoHz":1520,
    "mode":"USB", "bandIndex":30, "band":"HAM 20m",
    "rssi":45, "snr":22, "stereo":false, "volume":40,
    "bat":3.85, "batPct":72, "charging":false, "usbIn":false,
    "rdsName":"", "rdsProg":"", "agc":true, "agcGain":0,
    "sampleRate":12000, "dropped":0, "ts":12345678,
    "ntpSynced":true, "utcMs":1700000000000,   // UTC ms (only when NTP synced)
    "bands":[...] }

Text (client→server):  JSON commands
  { "cmd":"tune",      "freq":14074 }
  { "cmd":"mode",      "mode":"USB" }
  { "cmd":"band",      "index":21 }
  { "cmd":"volume",    "value":40 }
  { "cmd":"bfo",       "hz":50 }       // trim +-500 Hz
  { "cmd":"agc",       "enable":true, "gain":0 }
  { "cmd":"seek_up" }  { "cmd":"seek_down" }
  { "cmd":"step_up" }  { "cmd":"step_down" }
```

### REST API

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/status` | Full receiver status JSON |
| GET | `/api/bands` | All 36 bands with details |
| GET | `/api/ft8freqs` | FT8 dial frequencies |
| GET | `/api/ntp` | NTP server, sync state, UTC ms |
| POST | `/api/ntp` | `{"server":"pool.ntp.org"}` — set NTP server |
| POST | `/api/tune` | `{"freq":14074}` |
| POST | `/api/mode` | `{"mode":"USB"}` |
| POST | `/api/band` | `{"index":21}` |

---

## Getting Started

### Prerequisites
- PlatformIO + VS Code
- LILYGO T-Embed SI4732 board
- USB-C cable

### 1. Clone and optionally pre-configure Wi-Fi

The easiest way to set Wi-Fi credentials is via the **captive portal** at
runtime (see step 5). Alternatively, set compile-time defaults in
`platformio.ini` and they will be tried on first boot:
```ini
-DWIFI_SSID=\"your_network_name\"
-DWIFI_PASS=\"your_password\"
```
Credentials entered via the captive portal are saved to NVS and take
priority over compile-time defaults on subsequent boots.

### 2. Flash firmware
```bash
pio run --target upload
```

### 3. Upload web UI to LittleFS
```bash
pio run --target uploadfs
```
Both steps are required. The firmware serves a 404 without the filesystem image.

### 4. Enter flash mode (if port not detected)
Hold **BOOT** (encoder button) → press **RST** → release RST → release BOOT.

### 5. Configure Wi-Fi (first boot — captive portal)

On first boot (no saved credentials), the device creates the `T-Embed-Radio`
hotspot.

1. Connect your phone or laptop to **`T-Embed-Radio`** (password: `radiopass`)
2. A setup page opens automatically (captive portal). If it doesn't, navigate
   to **`http://192.168.4.1/wifi`**
3. Tap **Scan for networks**, select your network, enter the password, tap
   **Connect & Save**
4. The device reboots, joins your network, and the AP stays running

After connecting, the serial monitor shows both IPs:
```
STA connected. STA IP=192.168.1.42  AP IP=192.168.4.1
Radio UI: http://192.168.1.42  (also http://192.168.4.1)
```
The radio UI is reachable on **both** addresses simultaneously.

To clear saved credentials and re-run the portal, erase NVS:
```bash
pio run --target erase
```
then re-flash firmware and filesystem.

### 6. Verify I2C devices (first boot)

Read the serial monitor output. The I2C scanner will print the devices found
on the bus. Expected output with correct pin order (SDA=IO18, SCL=IO08):
```
I2C scan: 4 device(s) found — 0x40 (ES7210), 0x55 (BQ27220), 0x63 (SI4732), 0x6B (BQ25896)
```
If fewer than 4 devices are found, check that the SI4732 module is seated in
its JST connector and that `PWR_ON` (IO46) is driven HIGH at boot.

---

## Encoder Controls

| Action | FREQ target | VOL target | BFO target | BAND target |
|--------|------------|-----------|-----------|------------|
| **Rotate** | Tune frequency | Adjust volume | Trim BFO ±10 Hz/click | Next/prev band |
| **Short press** | *Cycle to next target* | → | → | → |
| **Long press** | Seek up | Toggle mute | — | — |
| **Double press** | — | — | Reset BFO trim to 0 | — |

In SSB/CW mode, encoder rotation tunes in 100 Hz steps. When the BFO
window (±500 Hz) is exceeded, the SI4732 chip retraces by 1 kHz and the
BFO wraps — producing seamless, jump-free audio tuning.

---

## FT8 / FT4 Decoding

Decoding runs entirely in the **browser** — no PC software or WASM compilation needed.

```
SI4732 tuned to FT8 dial frequency (USB mode)
SoftSSBDemod recovers USB audio → streams over WebSocket
       |
       v
ft8ts pure-TypeScript decoder in ft8worker.js (Module Web Worker)
  - Clock reference: ESP32 NTP UTC epoch fed every 500 ms via status frames
    correctedNow = espUtcMs + (Date.now() − receiveMs)
  - Waits for next UTC 15 s slot boundary, buffers a complete slot, decodes
  - Decodes FT8 and FT4 callsigns, grid squares, reports
       |
       v
Decoded spots shown in scrollable log (newest first)
```

**Time synchronisation (FT8):**

1. **NTP (automatic)** — ESP32 syncs to `pool.ntp.org` when STA is connected.
   UTC timestamp injected into every status frame for browser slot alignment.
2. **Custom NTP server** — set in the *NTP:* field; stored in NVS flash.
3. **Sync Now (manual)** — press at the moment you hear the first FT8 tones.

**Pre-configured FT8 dial frequencies (USB):**

| Band | Frequency |
|------|-----------|
| 80m | 3.573 MHz |
| 40m | 7.074 MHz |
| 30m | 10.136 MHz |
| 20m | 14.074 MHz |
| 17m | 18.100 MHz |
| 15m | 21.074 MHz |
| 10m | 28.074 MHz |

**Setting up ft8ts (required for FT8/FT4 callsign decoding):**

```bash
npm pack @e04/ft8ts              # downloads ft8ts-x.x.x.tgz
tar xzf ft8ts-*.tgz package/dist/ft8ts.mjs
cp package/dist/ft8ts.mjs data/js/
pio run -t uploadfs
```

Without `ft8ts.mjs` the worker falls back to audio energy analysis (signal
presence only). ft8ts is GPL-3.0 licensed.

---

## JS8Call Decoding

JS8Call decoding uses a **Rust WASM library** (`js8call-decoder/wasm/`) built
with wasm-pack. It implements the complete JS8Call receive chain:

- **Spectrogram** — FFT-based power spectrogram at 12 kHz
- **Costas sync search** — correlation over the full (time, freq) grid
- **LLR extraction** — soft log-likelihood ratios for LDPC input
- **LDPC(174,91) decoder** — belief propagation, same code as FT8
- **Message decode** — CRC-11, callsign pairs, grid squares, free text

```
SI4732 tuned to JS8Call dial frequency (USB mode, same audio stream)
       |
       v
js8call-worker.js (Module Web Worker)
  - Accumulates samples for one slot duration (see modes below)
  - Calls Js8Decoder.push_samples() + run_decode()
  - take_results() returns { freq_hz, snr_db, message } per decode
       |
       v
Decoded messages shown in JS8Call log (newest first)
```

**Speed modes** (selectable in the web UI):

| Mode | Slot | Symbol | FFT size | Tone spacing |
|------|------|--------|----------|--------------|
| Normal | 15.6 s | 160 ms | 1920-pt | 6.25 Hz |
| Fast | 10 s | 80 ms | 960-pt | 12.5 Hz |
| Turbo | 6 s | 40 ms | 480-pt | 25.0 Hz |
| Slow | 30 s | 320 ms | 3840-pt | 3.125 Hz |

> **Timing note:** The JS8Call WASM decoder does not yet use NTP-aligned slot
> boundaries — it counts samples from when you press Start. For best results,
> press **Start JS8Call** at the beginning of a transmission slot. NTP
> alignment will be added in a future update.

**Building the JS8Call WASM** (one-time, requires Rust + Node.js):

```bash
# Install Rust: https://rustup.rs
# Install wasm-pack: cargo install wasm-pack

cd js8call-decoder
npm install
npm run wasm:build            # → wasm/pkg/js8call_wasm.js + .wasm

# Copy the two output files to LittleFS
cp wasm/pkg/js8call_wasm.js     ../data/js/
cp wasm/pkg/js8call_wasm_bg.wasm ../data/js/

cd ..
pio run -t uploadfs
```

Without the WASM files, the JS8Call decoder falls back to RMS energy
reporting (signal presence only). The status line shows which mode is active.

---

## Dependencies

All installed automatically by PlatformIO from `platformio.ini`:

| Library | Version | Purpose |
|---------|---------|---------|
| `pu2clr/PU2CLR SI4735` | GitHub HEAD | SI4732 AM/FM/SW/LW control |
| `bodmer/TFT_eSPI` | ≥ 2.5.43 | ST7789 display driver |
| `mathieucarbou/ESPAsyncWebServer` | ≥ 3.3.0 | Async HTTP + WebSocket |
| `mathieucarbou/AsyncTCP` | ≥ 3.2.0 | Async TCP dependency |
| `fastled/FastLED` | ≥ 3.9.0 | APA102 RGB LED |
| `bblanchon/ArduinoJson` | ≥ 7.0.0 | JSON parsing |
| `madhephaestus/ESP32Encoder` | ≥ 0.10.2 | Rotary encoder |
| `lewisxhe/XPowersLib` | ≥ 0.2.6 | BQ25896 charger + power management |

> The PU2CLR library is sourced directly from GitHub
> (`https://github.com/pu2clr/SI4735.git`) because the PlatformIO registry
> entry has a name with spaces that the toolchain cannot resolve reliably.

> `mathieucarbou/ESPAsyncWebServer` is the actively maintained fork of the
> original `me-no-dev` library, required for ESP-IDF 4.4.x / Arduino-ESP32 2.x
> compatibility.

**Browser-side (loaded from LittleFS — obtain separately):**
- [`@e04/ft8ts`](https://github.com/e04/ft8ts) — pure TypeScript FT8/FT4 decoder
  (`npm pack @e04/ft8ts`, extract `dist/ft8ts.mjs` to `data/js/`, run `uploadfs`).
  GPL-3.0. No WASM compilation required.

---

## Build Status

| Module | File(s) | Status |
|--------|---------|--------|
| Pin configuration | `PinConfig.h` | ✅ Complete — all pins verified against schematic and pinmap |
| I2C auto-detection | `I2CScanner.h` | ✅ Complete — runs at every boot |
| Power management | `PowerManager.h/.cpp` | ✅ Complete — BQ25896 + BQ27220 + LiPo curve |
| Band configuration | `BandConfig.h` | ✅ Complete — 36 bands, FT8 table |
| Radio control | `RadioController.h/.cpp` | ✅ Complete — AM/FM/SW/LW/SSB/CW |
| Software SSB | `SoftSSBDemod.h/.cpp` | ✅ Complete — DDS BFO + biquad LPF |
| Audio capture | `AudioCapture.h/.cpp` | ✅ Complete — ADC DMA, ADC cal, IIR DC-blocker, software AGC |
| FFT waterfall | `FFTProcessor.h/.cpp` | ✅ Complete — PSRAM buffers |
| WebSocket server | `WebSocketHandler.h/.cpp` | ✅ Complete — idle throttle |
| REST API / NTP | `WebServer.cpp` | ✅ Complete — NTP, modem sleep, deferred reboot |
| Local TFT display | `DisplayManager.h/.cpp` | ✅ Complete — S-meter, auto-dim/sleep |
| Encoder input | `EncoderHandler.h/.cpp` | ✅ Complete — wakes display on input |
| FreeRTOS main | `main.cpp` | ✅ Complete |
| Web UI HTML/CSS | `index.html`, `style.css` | ✅ Complete |
| Audio player | `audio.js` | ✅ Complete — AudioWorklet + browser BFO |
| Waterfall renderer | `waterfall.js` | ✅ Complete — 3 palettes |
| FT8/FT4 decoder | `ft8.js` | ✅ Complete — NTP UTC alignment, 15 s slot |
| FT8 decode worker | `ft8worker.js` | ✅ Complete — module worker, ft8ts + energy fallback |
| ft8ts decoder | `ft8ts.mjs` | ⬇ Download separately from `@e04/ft8ts` npm package |
| JS8Call controller | `js8call.js` | ✅ Complete — 4 speed modes, WASM + energy fallback |
| JS8Call worker | `js8call-worker.js` | ✅ Complete — module worker, LDPC decode |
| JS8Call WASM | `js8call_wasm.js` + `.wasm` | ⬇ Build from `js8call-decoder/` with wasm-pack |
| CW decoder | `cw.js` | ✅ Complete — Goertzel, adaptive threshold, auto-WPM, ITU table |
| UI controller | `app.js` | ✅ Complete — S-meter, NTP status, JS8Call/CW routing |

---

## Known Limitations

**SI4732 analog audio level unverified.** The ADC input expects 0–3.3V; the
SI4732 audio output is a line-level signal centred at VCC/2. If the received
audio is very quiet, an op-amp gain stage between the SI4732 output and IO17
may be needed. Verify with a multimeter or oscilloscope on first boot.

**Software SSB audio quality.** The product detector approach produces good
intelligible audio for voice and FT8. It is not as clean as the hardware SSB
patch for very weak signals, because the AM filter preceding the product
detector passes some adjacent interference that hardware SSB would reject.
The SI4732's 3 kHz AM bandwidth filter is the best available without the patch.

**ESP32-S3 ADC effective resolution limits weak-signal FT8.** The theoretical
12-bit ADC gives 72 dB SINAD, but the ESP32-S3 achieves roughly 9–10 ENOB
(~54–60 dB effective) in practice. ADC calibration (`esp_adc_cal`) corrects
the INL bow but not wideband noise. Strong FT8 signals decode reliably;
signals below approximately −10 dB SNR may be missed compared to a dedicated
sound card. WSPR (requires −28 dB SNR) is not practical with this path.

**Waterfall shows audio spectrum, not RF.** The waterfall displays 0–6 kHz
audio content within the tuned filter bandwidth — not a panoramic RF spectrum
view. For FT8 and CW this is exactly what the decoders need.

**ft8ts.mjs not included.** Must be obtained from the
[`@e04/ft8ts`](https://github.com/e04/ft8ts) npm package and placed in
`data/js/` before running `pio run -t uploadfs`. Without it, the FT8/FT4
decoder falls back to RMS energy reporting (signal presence only, no
callsigns).

**JS8Call WASM not pre-built.** Must be compiled from the included
`js8call-decoder/` project using Rust + wasm-pack (see *JS8Call Decoding*
section). Without `js8call_wasm.js` / `js8call_wasm_bg.wasm` in `data/js/`,
the JS8Call decoder falls back to RMS energy reporting. JS8Call message
decoding handles standard callsign pairs, grid squares, and free text; some
JS8Call-specific directed and relay message types may decode as `<bits:hex>`
until full message-type coverage is added.

**JS8Call slot boundaries not NTP-aligned.** The JS8Call decoder counts
samples from when Start is pressed rather than locking to the UTC slot grid.
Press Start at the beginning of a transmission for best decode rate. FT8 is
unaffected — it uses full NTP alignment.

---

## References

- [LILYGO T-Embed SI4732 product page](https://lilygo.cc/products/t-embed-si4732)
- [LILYGO T-Embed GitHub](https://github.com/Xinyuan-LilyGO/T-Embed)
- [LILYGO T-Embed SI4732 Wiki](https://wiki.lilygo.cc/get_started/en/Wearable/T-Embed-SI4732/T-Embed-SI4732.html)
- [PU2CLR SI4735 Arduino Library](https://github.com/pu2clr/SI4735)
- [lewisxhe/XPowersLib](https://github.com/lewisxhe/XPowersLib) — BQ25896/BQ27220
- [mathieucarbou/ESPAsyncWebServer](https://github.com/mathieucarbou/ESPAsyncWebServer)
- [goshante/ats20_ats_ex](https://github.com/goshante/ats20_ats_ex) — SSB reference
- [esp32-si4732 org](https://github.com/esp32-si4732) — ATS-Mini community firmware
- [e04/ft8ts](https://github.com/e04/ft8ts) — pure TypeScript FT8/FT4 decoder (GPL-3.0)
- `js8call-decoder/` (included) — custom Rust WASM JS8Call decoder (LDPC + callsign decode)
- [SI4735 Programming Guide AN332](https://www.silabs.com/documents/public/application-notes/AN332.pdf)

---

## License

MIT — see `LICENSE`.

The PU2CLR SI4735 library is MIT licensed. XPowersLib is MIT licensed.
No proprietary Silicon Labs patch binary is used or distributed in this project.
