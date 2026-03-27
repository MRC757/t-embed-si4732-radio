# LILYGO T-Embed SI4732 — Web-Enabled HAM Radio Receiver

A complete firmware for the **LILYGO T-Embed SI4732** that turns it into a
web-connected multiband radio receiver with a browser-based control interface,
real-time S-meter, NTP time sync, OTA updates, and 10 memory channels.

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
- **LSB / USB (SSB)** — all HAM bands 160m through 10m *(chip configured; see SSB status below)*
- **CW** — 700 Hz BFO offset *(chip configured; see SSB status below)*
- **36 pre-configured bands** including all major HAM, broadcast, CB, Marine HF, and WWV allocations

> **Out-of-range note:** GMRS (462 MHz), FRS (462–467 MHz), MURS (151–154 MHz),
> and VHF Air (118–136 MHz) are **above the SI4732's 30 MHz upper limit** and
> cannot be received with this hardware.

### SSB — Current Status

**SSB and CW do not produce usable audio on the speaker with current hardware.**

Two separate limitations prevent SSB from functioning:

#### 1. Speaker path bypasses the ESP32

The speaker is wired directly from the SI4732 analog output through its on-module
amp. The ESP32 controls only the mute pin (IO17). It has no role in the audio
signal itself:

```
SI4732 LOUT ──► analog amp ──► speaker
                    │
              IO17 (mute only)
```

When SSB or CW mode is selected, the SI4732 is placed in **AM mode** with a
tuning offset so the sideband falls in the 3 kHz AM passband. However, AM
detection of a suppressed-carrier SSB signal without carrier reinsertion produces
distorted, unintelligible audio at the speaker — a "Donald Duck" effect.

The software product detector (`SoftSSBDemod`) only processes audio in the ESP32's
capture pipeline, which feeds the web stream — it cannot affect the speaker path.

#### 2. Web stream audio capture is non-functional

`SoftSSBDemod` is fully implemented (DDS BFO oscillator, 4th-order Butterworth
LPF) and would correctly demodulate SSB for the web stream — but `AudioCapture`
has no audio input. Both software attempts to capture audio have failed:

- **IO17 ADC:** SI4732 LOUT DC bias (~1.65 V) mutes the speaker amp when IO17 is
  high-impedance ADC input — the two uses are physically incompatible
- **I2S slave RX (IO06/05/07):** SI4732 digital audio pins not wired to the
  Speaker Slot connector on this PCB — `i2s_read()` times out with no data

#### What is working in SSB mode

- SI4732 chip tuned correctly (AM mode, narrow bandwidth, tuning offsets for LSB/USB)
- AGC disabled, soft mute disabled for SSB — correct chip configuration
- BFO frequency display and fine-tune controls functional
- Frequency display with 100 Hz step resolution functional
- `SoftSSBDemod` code correct and ready — activates automatically once audio
  capture is established via a hardware modification (see Web Audio Options)

#### Path to working SSB

SSB via the web stream will work once a hardware audio tap is added (Option 2a
jumper wire or Option 4 ES7210 MIC3 mod — see Web Audio Options section).

Speaker SSB requires either the SI4732 hardware SSB patch (binary blob, not used
in this firmware) or a hardware modification to route audio through the ESP32's
DAC back to the speaker amplifier.

LSB and USB use different SI4732 chip tuning offsets so the sideband falls
correctly in the AM passband. See `BandConfig.h` for the tuning notes.

### Web Interface
- Live frequency display and tuning controls
- Band and mode selector with all 36 bands
- **S-meter** — RSSI displayed as S1–S9 / S9+dB (S9 = 34 dBμV, 6 dB/unit),
  with **peak-hold** tick (3 s hold, 1 unit/200 ms decay)
- SNR bar and RDS display for FM stations
- Volume, AGC, and BFO trim controls
- **Memory channels** — 10 named NVS slots; save/load from web UI
- **NTP time sync** — ESP32 NTP client feeds accurate UTC to the browser;
  configurable server via web GUI; **Sync Now** button for manual alignment
  when internet NTP is unavailable

### Settings Persistence
- **NVS flash** — frequency, band, mode, and volume saved on every change
  (debounced 2 s) and restored on boot via `Preferences` namespace `"radio"`
- **RTC memory** — `RTC_DATA_ATTR` statics restored instantly after deep sleep
  wake without touching NVS flash
- **Memory channels** — 10 slots in NVS namespace `"mem"`; each stores
  frequency, band, mode, and a user label

### OTA Firmware Updates
`ArduinoOTA` is enabled after STA connect; hostname `t-embed-radio`.

```bash
pio run -t upload --upload-port t-embed-radio.local
```

No USB cable needed once the device is on Wi-Fi.

### Power Management
- **BQ25896** USB-C battery charger — initialised at boot so the 900 mAh
  LiPo actually charges when USB-C is connected
- **BQ27220** battery fuel gauge — accurate state-of-charge %, voltage,
  current, and cycle count over I2C
- Battery fallback (gauge absent): 13-point LiPo discharge curve lookup
  instead of a linear approximation
- Battery % and charging indicator on the local TFT display and web UI
- **Wi-Fi modem sleep** — disabled; on ESP32-S3 modem sleep triggers periodic
  RF calibration that preempts ADC1, causing audio stream dropouts
- **WebSocket stream throttle** — stream task drops to 100 ms loop period
  when no browser clients are connected (vs 20 ms active), saving CPU

### Hardware UI
- 1.9" ST7789V IPS TFT — frequency, mode, band, S-meter bar with peak tick,
  RDS, battery, and **UTC clock** (HH:MM:SS — shown after NTP sync)
- **Auto-dim / auto-sleep** — TFT backlight dims after 60 s idle, turns off
  after 5 min; any encoder or button input restores full brightness
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
| Audio (speaker) | SI4732 → analog amp → speaker (direct hardware path) |
| Speaker mute | IO17 — LOW=playing, HIGH=muted (GPIO output) |
| Audio (web stream) | **Pending hardware mod** — IO17 ADC (Option 1) and I2S slave RX (Option 3) both tested and failed |
| I2S amp | MAX98357A on IO07/05/06 — I2S slave RX tested (Option 3): SI4732 digital pins not wired to this connector |
| Microphone | ES7210 I2S ADC (IO47/21/14/48) |
| LED | APA102 RGB (IO45/42) |
| Storage | MicroSD SPI (IO39/40/41/38) |
| Charger | BQ25896 (I2C 0x6B) |
| Fuel gauge | BQ27220 (I2C 0x55) |
| Power | USB-C or 3.7V LiPo 900 mAh |

### Pin Map

> All pins verified from the official LILYGO T-Embed SI4732 product pinmap image
> and confirmed by physical inspection of the GPIO expansion port.

| Peripheral | Pins | Notes |
|------------|------|-------|
| ST7789 LCD | BL=IO15, DC=IO13, CS=IO10, CLK=IO12, MOSI=IO11, RST=IO09 | |
| Encoder | A=IO02, B=IO01, Button=IO00 (BOOT) | |
| I2S Speaker amp | BCLK=IO07, WCLK=IO05, DOUT=IO06 | MAX98357A — I2S slave RX tested here (Option 3): no SI4732 digital audio clocks received; pins appear unconnected to SI4732 module |
| ES7210 Mic ADC | BCLK=IO47, LRCK=IO21, DIN=IO14, MCLK=IO48 | MIC3/4 unconnected — ES7210 upgrade candidate |
| APA102 LED | CLK=IO45, DATA=IO42 | |
| MicroSD | CS=IO39, SCLK=IO40, MOSI=IO41, MISO=IO38 | |
| SI4732 I2C | SDA=IO18, SCL=IO08 | |
| SI4732 RST | IO16 | Confirmed by physical expansion port inspection |
| SI4732 Audio / Mute | IO17 | **MUTE pin** (LOW=playing, HIGH=muted); also SI4732 LOUT ADC tap — mutually exclusive uses |
| SI4732 Power | IO46 (active HIGH) | |
| BQ25896 / BQ27220 | I2C 0x6B / 0x55 | Same bus as SI4732 |

> **IO17 note:** The official pinmap labels this pin `AUDIO`. Physical inspection of
> the expansion port confirms it as `SD` (shutdown/mute for the onboard analog amp).
> The diy-ovilus-firmware for the same board uses it as `PIN_AUDIO_MUTE` with
> `LOW=playing, HIGH=muted`. The SI4732 LOUT drives the speaker through the analog
> amp directly — the ESP32 only controls the mute line.
>
> **Option 1 tested and FAILED:** IO17 as ADC input (high-Z) lets the SI4732 LOUT
> DC bias (~1.65 V) reach the amp shutdown pin. 1.65 V is above the amp's mute
> threshold — the speaker goes silent. The two uses (mute GPIO output and ADC
> audio input) are physically incompatible on this pin.

> I2C pin order confirmed from the official LILYGO pinmap photo (SDA=IO18, SCL=IO08).
> The boot-time `I2CScanner` still runs and will report if a hardware variant is
> encountered with the opposite order.

#### Full I2C Bus

All four devices share one bus (400 kHz):

| Device | Address | Purpose |
|--------|---------|---------|
| SI4732 | 0x63 (SEN=VCC) or 0x11 (SEN=GND) | Radio tuner |
| ES7210 | 0x40 | Microphone ADC |
| BQ27220 | 0x55 | Battery fuel gauge |
| BQ25896 | 0x6B | USB-C battery charger |

---

## Web Audio Options — Tested Results

Two software-only approaches have been tested and failed. A hardware modification
is required to add web audio streaming. Three options remain, in order of effort:

### Option 1 — IO17 ADC tap: **TESTED — FAILED**

IO17 is the analog amp MUTE pin. When configured as ADC input (high-Z), the
SI4732 LOUT DC bias (~1.65 V) reaches the amp shutdown pin and mutes the
speaker. The two functions cannot coexist on this pin.

### Option 3 — I2S Slave RX from SI4732 digital output: **TESTED — FAILED**

The SI4732-A10 has digital audio output pins (DOUT/DFS/DCLK). Firmware was
written to receive these as I2S slave on IO06/IO05/IO07 (the MAX98357A speaker
amplifier slot). `i2s_read()` timed out on every call — no I2S clocks were
received in 77+ seconds of operation. The SI4732 digital audio pins are **not
wired through** the Speaker Slot connector to IO06/05/07 on this module PCB.

### Option 2a — GPIO3 or GPIO4 jumper wire from SI4732 LOUT tap (no soldering to IC)

Find the SI4732 LOUT test pad or headphone jack contact on the module PCB.
Wire it through a 100 nF AC-coupling cap to any free ADC GPIO (GPIO3 or
GPIO4). Configure that pin as ADC input in AudioCapture; IO17 remains as
speaker mute output. This approach requires finding the right test pad on the
module PCB with a multimeter.

### Option 4 — ES7210 MIC3 hardware mod (best quality — requires IC-level soldering)

The ES7210 quad-channel I2S ADC is already on the board. Only two of its four
input channels (MIC1, MIC2) are used by the MEMS microphones. MIC3 and MIC4
are left unconnected on the PCB, making MIC3 an ideal tap for SI4732 audio.

This upgrade gives 24-bit I2S audio (vs. 12-bit ADC) and eliminates the
IO17 conflict entirely — speaker mute and web stream can operate simultaneously.

### Option 4 Hardware modification (ES7210 MIC3)

| Point | Location | Notes |
|-------|----------|-------|
| **Source** | SI4732 LOUT tap on module PCB | ~1.65 V DC bias + audio signal |
| **Destination** | ES7210 **MIC3P** (IC pin 31) | Requires soldering to the ES7210 QFN pad |
| **MIC3N** | ES7210 pin 32 | Tie to AGND (analog ground) |
| **MICBIAS34** | ES7210 pin 26 | Already has bypass caps — no work needed |
| **REFP34** | ES7210 pin 29 | Already has bypass cap — no work needed |

**Required component:** one **100 nF** ceramic capacitor in series for AC
coupling (removes the 1.65 V DC bias from SI4732 LOUT before the ES7210 input).

```
SI4732 LOUT (tap on module PCB)
        |
       [100nF]          ← AC coupling — removes 1.65 V DC bias
        |
    ES7210 MIC3P (pin 31)
    ES7210 MIC3N (pin 32) ── AGND
```

### Option 4 Firmware changes required

Once the hardware is wired:

1. **`main.cpp`** — keep `PIN_AUDIO_MUTE` LOW (speaker stays unmuted); replace
   the `audioCapture.begin()` call with the ES7210 I2S path.
2. **`AudioCapture.cpp`** — replace the I2S slave RX loop with an I2S master RX
   read from `I2S_PORT_MIC` (`I2S_NUM_0`). Configure the ES7210 driver
   (lewisxhe ES7210.h) for single-channel 24-bit capture on channel 2 (MIC3).
3. **`AudioCapture.h`** — remove I2S slave constants; add `ES7210_MIC_CHANNEL 2`.
4. **Sample rate** — ES7210 supports 8 / 12 / 16 / 44.1 / 48 kHz. Use 12 kHz
   to match the stream rate and ft8_lib decoder natively.

> The speaker path (SI4732 → analog amp → IO17 mute → speaker) is unaffected
> and continues to work independently.

---

## Audio Architecture

### Speaker path (confirmed working)

The SI4732 drives the speaker through its own **analog amp** on the module PCB.
The ESP32 only controls the mute line — no I2S or ADC involvement needed.

```
SI4732 LOUT ──► analog amp (on SI4732 module) ──► speaker
                      │
               IO17 (MUTE pin)
               LOW  = playing   ← firmware drives this
               HIGH = muted
```

The speaker works at any SI4732 volume level (0–63). Volume 63 is set at boot.

### Web stream path (pending hardware modification)

Two software-only approaches were tested and failed (see Web Audio Options section):

- **Option 1 (IO17 ADC):** FAILED — 1.65 V SI4732 LOUT bias mutes the amp when IO17 is ADC input.
- **Option 3 (I2S slave RX on IO06/05/07):** FAILED — SI4732 digital audio not wired to Speaker Slot connector.

`AudioCapture.cpp` currently contains the Option 3 I2S slave RX code. It
compiles and initialises successfully (i2s_driver_install succeeds) but
`i2s_read()` never receives data because no I2S clocks arrive from the SI4732.
The web audio stream and FFT waterfall are therefore inactive.

Once a hardware audio tap is established (Option 2a jumper wire or Option 4
ES7210 mod), the signal processing chain will be:

```
SI4732 LOUT (via hardware tap — Option 2a GPIO or Option 4 ES7210 MIC3)
       |
       v
AudioCapture — I2S or ADC read → 12 kHz mono int16 PCM
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
             |
             +-- FFTProcessor  --> waterfall bins --> /ws/radio
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
│   ├── RadioController.h/.cpp SI4732 via PU2CLR; AM/FM/SW/LW/SSB/CW;
│   │                          NVS + RTC persistence; 10 memory channels;
│   │                          S-meter peak hold
│   ├── BandConfig.h           36 bands, FT8 quick-tune table, SSB tuning notes
│   └── SSBPatch.h             Stub (patch approach replaced by SoftSSBDemod)
├── dsp/
│   ├── SoftSSBDemod.h/.cpp    Software product detector (no patch needed)
│   ├── FFTProcessor.h/.cpp    Hann-windowed FFT → waterfall magnitude bins
├── audio/
│   └── AudioCapture.h/.cpp    I2S slave RX (Option 3 code — non-functional; hardware tap pending)
├── web/
│   ├── WebServer.cpp          Wi-Fi AP+STA, captive portal, NTP, OTA, REST API
│   └── WebSocketHandler.h/.cpp /ws/audio PCM + /ws/radio waterfall+status+memory
├── display/
│   └── DisplayManager.h/.cpp  Sprite TFT UI, S-meter + peak tick, UTC clock, auto-dim/sleep
├── input/
│   └── EncoderHandler.h/.cpp  Rotary encoder, calls wakeDisplay()
└── main.cpp                   FreeRTOS tasks, I2CScanner, PowerManager init,
                               reset-reason logging, stack HWM diagnostics
data/                          LittleFS web UI (pio run -t uploadfs)
├── index.html
├── css/style.css
└── js/
    └── app.js                 UI controller — tuning, S-meter + peak, NTP, memory channels
js8call-decoder/               Rust WASM source for JS8Call decoder (future use)
├── wasm/src/                  Rust source (ldpc.rs, sync.rs, message.rs, ...)
├── tools/gen_ldpc_matrix.py   LDPC matrix generator (fetches from ft8lib)
└── package.json               Build scripts (npm run wasm:build)
js8call-wasm-decoder/          Standalone release of the JS8Call decoder
├── dist/                      Pre-compiled WASM + browser JS
├── wasm/src/                  Editable Rust source
└── README.md                  Self-contained build and integration guide
BUILD_JS8CALL_WASM.md          Step-by-step WASM build instructions with troubleshooting
```

### FreeRTOS Tasks

| Task | Core | Priority | Purpose |
|------|------|----------|---------|
| AudioI2S | 1 | 6 | I2S slave RX → SoftSSBDemod → AGC → ring buffer *(non-functional — SI4732 digital audio not wired to IO06/05/07)* |
| RadioCtrl | 1 | 5 | SI4732 RSSI/RDS poll, I2C watchdog, S-meter peak hold |
| FFTDsp | 1 | 4 | FFT waterfall rows *(inactive — no audio input)* |
| Encoder | 0 | 4 | Rotary encoder debounce |
| WSStream | 0 | 3 | WebSocket audio + waterfall broadcast *(silent — no audio input)* |
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

Text (server→client):  JSON status @ 2 Hz
  { "type":"status",
    "freq":14074,          // dial frequency kHz
    "freqHz":14074000,     // display frequency Hz (dial + BFO fine-tune for SSB)
    "chipKHz":14074,       // SI4732 chip tuning frequency kHz
    "bfoHz":1520,
    "mode":"USB", "bandIndex":30, "band":"HAM 20m",
    "rssi":45, "rssiPeak":48,   // current RSSI + peak hold value
    "snr":22, "stereo":false, "volume":40,
    "bat":3.85, "batPct":72, "charging":false, "usbIn":false,
    "rdsName":"", "rdsProg":"", "agc":true, "agcGain":0,
    "sampleRate":12000, "dropped":0, "ts":12345678,
    "ntpSynced":true, "utcMs":1700000000000 }

Text (client→server):  JSON commands
  { "cmd":"tune",      "freq":14074 }
  { "cmd":"mode",      "mode":"USB" }
  { "cmd":"band",      "index":21 }
  { "cmd":"volume",    "value":40 }
  { "cmd":"bfo",       "hz":50 }         // trim +-500 Hz
  { "cmd":"agc",       "enable":true, "gain":0 }
  { "cmd":"seek_up" }  { "cmd":"seek_down" }
  { "cmd":"step_up" }  { "cmd":"step_down" }
  { "cmd":"mem_save",  "slot":0, "label":"40m FT8" }
  { "cmd":"mem_load",  "slot":0 }
  { "cmd":"mem_list" }                   // server replies with mem_list JSON
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

### 2. Set the upload port

Find the COM port assigned to the T-Embed:
```bash
pio device list
```

Edit `platformio.ini` and update the `upload_port` line:
```ini
upload_port = COM3    ; change to match your system
```

### 3. Flash firmware
```bash
pio run --target upload
```

### 4. Upload web UI to LittleFS
```bash
pio run --target uploadfs
```
Both steps are required. The firmware serves a 404 without the filesystem image.

### 5. Enter flash mode (if port not detected)
Hold **BOOT** (encoder button) → press **RST** → release RST → release BOOT.

### 6. Configure Wi-Fi (first boot — captive portal)

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

### 7. OTA firmware updates (after first Wi-Fi setup)

Once the device is on your network, subsequent firmware flashes can be done
wirelessly:
```bash
pio run -t upload --upload-port t-embed-radio.local
```
The device must be powered on and STA-connected. The TFT shows `OTA…` during
the update and reboots automatically when complete.

### 8. Verify I2C devices (first boot)

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

---

## Build Status

| Module | File(s) | Status |
|--------|---------|--------|
| Pin configuration | `PinConfig.h` | ✅ Complete — all pins verified against schematic and pinmap |
| I2C auto-detection | `I2CScanner.h` | ✅ Complete — runs at every boot |
| Power management | `PowerManager.h/.cpp` | ✅ Complete — BQ25896 + BQ27220 + LiPo curve |
| Band configuration | `BandConfig.h` | ✅ Complete — 36 bands, FT8 table |
| Radio control | `RadioController.h/.cpp` | ✅ Complete — AM/FM/SW/LW/SSB/CW; NVS + RTC persistence; 10 memory channels; S-meter peak hold |
| Software SSB | `SoftSSBDemod.h/.cpp` | ✅ Code complete — DDS BFO + biquad LPF; inactive until audio capture hardware mod is done; does not affect speaker path |
| Audio capture | `AudioCapture.h/.cpp` | ⚠️ Non-functional — I2S slave RX code present (Option 3); SI4732 digital audio not wired to IO06/05/07; hardware tap required |
| FFT waterfall | `FFTProcessor.h/.cpp` | ✅ Complete — PSRAM buffers *(inactive — no audio input)* |
| WebSocket server | `WebSocketHandler.h/.cpp` | ✅ Complete — idle throttle; memory channel support |
| REST API / NTP / OTA | `WebServer.cpp` | ✅ Complete — NTP, modem sleep, OTA, deferred reboot |
| Local TFT display | `DisplayManager.h/.cpp` | ✅ Complete — S-meter + peak tick, UTC clock, auto-dim/sleep |
| Encoder input | `EncoderHandler.h/.cpp` | ✅ Complete — wakes display on input |
| FreeRTOS main | `main.cpp` | ✅ Complete — reset-reason logging, stack HWM diagnostics |
| Web UI HTML/CSS | `index.html`, `style.css` | ✅ Complete |
| UI controller | `app.js` | ✅ Complete — S-meter + peak, NTP status, memory channels |

---

## Known Limitations

**Web audio stream requires hardware modification.** Two software-only options
were tested and failed: IO17 ADC tap (Option 1 — amp muted by 1.65 V LOUT bias)
and I2S slave RX on IO06/05/07 (Option 3 — SI4732 digital audio not wired to
that connector). Options remaining: GPIO3/4 jumper wire from a LOUT tap on the
module PCB (Option 2a), or ES7210 MIC3 solder mod (Option 4). Until one of
these is implemented, the waterfall and audio stream produce no output.

**SSB and CW do not produce usable audio on the speaker.** The SI4732 is placed
in AM mode when SSB/CW is selected. Its AM detector cannot demodulate a
suppressed-carrier SSB signal without carrier reinsertion — the speaker output is
distorted and unintelligible. The software product detector (`SoftSSBDemod`) is
in the web stream audio pipeline, not the speaker path, so it cannot help until a
hardware audio tap is added. See the SSB section for full details and the path
forward.

**Browser decoders (FT8, JS8Call, CW) require audio.** These decoder modules
are not included in the current web UI. They can be added back once a working
audio tap is established. The JS8Call WASM source remains available in
`js8call-decoder/` and `js8call-wasm-decoder/` for future integration.

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
- `js8call-decoder/` (included) — Rust WASM JS8Call decoder source (LDPC + callsign decode)
- [SI4735 Programming Guide AN332](https://www.silabs.com/documents/public/application-notes/AN332.pdf)

---

## License

MIT — see `LICENSE`.

The PU2CLR SI4735 library is MIT licensed. XPowersLib is MIT licensed.
No proprietary Silicon Labs patch binary is used or distributed in this project.
