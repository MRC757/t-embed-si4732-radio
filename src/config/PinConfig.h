#pragma once
// ============================================================
// PinConfig.h — LILYGO T-Embed SI4732
// Single source of truth for all GPIO pin assignments.
//
// Verified against the official LILYGO T-Embed SI4732 PINMAP
// image. Every pin assignment confirmed from the product photo.
//
// IMPORTANT ARCHITECTURE NOTE — SI4732 AUDIO:
// The SI4732-A10 on this board uses ANALOG audio output only.
// The analog audio is routed to IO17 (ADC-capable GPIO).
// There are NO I2S digital audio pins (DOUT/DFS/DCLK) exposed
// in the T-Embed SI4732 hardware pinmap.
//
// Audio capture uses ESP32-S3 ADC continuous DMA on IO17.
// This gives 12-bit, ~40kHz effective sample rate — adequate
// for SSB voice, FT8, WSPR, and all digital HAM modes.
//
// TWO I2C BUSES — do not confuse:
//   Internal bus: SDA=IO18, SCL=IO08
//     → SI4732 radio chip (address 0x63)
//     → ES7210 microphone ADC (address 0x40)
//   GPIO expansion header: SDA=IO16, SCL=IO17
//     → User breakout only — NOT connected to SI4732/ES7210
//     → IO17 also serves as SI4732 AUDIO (analog) — do not
//       use IO17 as I2C SCL if audio capture is active.
// ============================================================

// --- ST7789V 1.9" IPS TFT LCD (SPI) ---
#define PIN_LCD_BL      15  // Backlight PWM
#define PIN_LCD_DC      13  // Data/Command
#define PIN_LCD_CS      10  // Chip Select
#define PIN_LCD_CLK     12  // SPI Clock
#define PIN_LCD_MOSI    11  // SPI MOSI
#define PIN_LCD_RES      9  // Reset

// --- Rotary Encoder ---
#define PIN_ENC_A        2  // Encoder channel A
#define PIN_ENC_B        1  // Encoder channel B
#define PIN_ENC_BTN      0  // Encoder push button (BOOT pin)

// --- I2S Speaker Output (MAX98357A amplifier) ---
// Drives the onboard MAX98357A I2S class-D amplifier.
// SD_MODE is connected to IO17 (same pin as SI4732 LOUT and ADC audio input).
// The SI4732 LOUT DC bias (~1.65V at rest) passively holds SD_MODE above the
// 1.4V enable threshold — the amp is always on when SI4732 is powered.
// DO NOT drive IO17 as a GPIO output: it conflicts with the ADC input and
// would back-drive the SI4732 LOUT pin. No firmware SD_MODE control needed.
#define PIN_I2S_SPK_BCLK  7   // I2S Bit Clock
#define PIN_I2S_SPK_WCLK  5   // I2S Word/LR Clock
#define PIN_I2S_SPK_DOUT  6   // I2S Data Out → MAX98357A DIN
// PIN_AMP_MODE: SD_MODE = IO17 (passive, controlled by SI4732 LOUT bias — not a GPIO output)

// --- ES7210 Microphone ADC (I2S input from MEMS mic) ---
// ES7210 is a quad-channel I2S microphone ADC.
// It captures the onboard MEMS microphone, NOT SI4732 audio.
// I2C control shares the internal bus with the SI4732.
#define PIN_ES7210_BCLK   47  // I2S Bit Clock
#define PIN_ES7210_LRCK   21  // I2S LR Clock
#define PIN_ES7210_DIN    14  // I2S Data In (from ES7210)
#define PIN_ES7210_MCLK   48  // Master Clock
// ES7210 I2C control is on the internal bus (IO18/IO08)
#define ES7210_I2C_ADDR  0x40 // ES7210 default I2C address

// --- SI4732-A10 Radio (I2C control) ---
// The SI4732 module plugs into the "Speaker Slot" JST connector.
// I2C is the ONLY control interface. Audio output is ANALOG.
#define PIN_SI4732_SDA    18  // I2C SDA — internal bus
#define PIN_SI4732_SCL     8  // I2C SCL — internal bus (IO08, NOT IO17)
#define PIN_SI4732_RST    16  // RST → GPIO16 on the Speaker Slot JST connector
#define PIN_SI4732_PWR    46  // Module power enable — active HIGH (Power_On)

// SI4732 I2C address: SEN=VCC on T-Embed module → 0x63 (confirmed)
// If you get "device not found", try 0x11 (SEN=GND variant)
#define SI4732_I2C_ADDR_SEN_HIGH  0x63  // SEN tied to VCC (T-Embed default)
#define SI4732_I2C_ADDR_SEN_LOW   0x11  // SEN tied to GND (alternate)
#define SI4732_I2C_ADDR  SI4732_I2C_ADDR_SEN_HIGH

// --- SI4732 Analog Audio / Speaker Mute ---
// IO17 serves dual duty on the T-Embed SI4732:
//   1. SI4732 LOUT analog audio signal (readable via ADC1_CH6 for web stream)
//   2. Analog speaker amp MUTE pin (GPIO output: LOW=playing, HIGH=muted)
//
// These two uses are mutually exclusive — IO17 cannot be simultaneously
// an ADC input and a GPIO output.  Choose one at a time:
//   Speaker path:   pinMode(17, OUTPUT); digitalWrite(17, LOW);
//   Web stream:     configure ADC1_CH6 on IO17 (AudioCapture)
//
// Confirmed by physical inspection of the expansion port and by the
// diy-ovilus-firmware reference (PIN_AUDIO_MUTE=17, LOW=unmuted).
#define PIN_SI4732_AUDIO  17   // SI4732 LOUT → ADC1_CH6 (web stream, mutually exclusive with mute)
#define PIN_AUDIO_MUTE    17   // Analog amp MUTE: LOW=playing, HIGH=muted

// If the LilyGO schematic (not yet confirmed) shows SI4732 digital
// audio pins are actually wired, define them here and set
// SI4732_DIGITAL_AUDIO_AVAILABLE to 1 in AudioCapture.h.
// Until confirmed by schematic review, analog ADC path is used.
#define PIN_SI4732_DOUT   -1  // Not confirmed in pinmap (was wrongly IO06)
#define PIN_SI4732_DFS    -1  // Not confirmed in pinmap (was wrongly IO05)
#define PIN_SI4732_DCLK   -1  // Not confirmed in pinmap (was wrongly IO07)

// ============================================================
// I2C Bus Configuration
// ============================================================
//
// ✓  PIN ORDER CONFIRMED — from T-Embed-SI4732.jpeg pinmap and PU2CLR library example
//
// Two conflicting sources exist for SDA/SCL assignment:
//
//   Source A (product pinmap image, SI4732 module perspective):
//     SDA = IO18,  SCL = IO08
//
//   Source B (T-Embed CC1101 sister board working code):
//     SDA = IO8,   SCL = IO18  (Wire.begin(8, 18))
//
// The I2CScanner in src/power/I2CScanner.h runs at boot and
// tries BOTH configurations, reporting which one finds devices.
// Expected devices:  0x40 (ES7210), 0x55 (BQ27220),
//                    0x63 (SI4732),  0x6B (BQ25896)
//
#define I2C_SDA   18   // Confirmed: matches T-Embed-SI4732.jpeg pinmap
#define I2C_SCL    8   // Confirmed: matches T-Embed-SI4732.jpeg pinmap
#define I2C_FREQ  400000  // 400 kHz Fast Mode

// All four I2C devices share the same bus:
//   SI4732  radio tuner       0x63 (SEN=VCC) or 0x11 (SEN=GND)
//   ES7210  microphone ADC    0x40
//   BQ27220 battery gauge     0x55
//   BQ25896 battery charger   0x6B
#define I2C_ADDR_SI4732   0x63
#define I2C_ADDR_ES7210   0x40
#define I2C_ADDR_BQ27220  0x55
#define I2C_ADDR_BQ25896  0x6B

// GPIO expansion header — separate user breakout, NOT used internally
#define PIN_EXP_SDA  16
#define PIN_EXP_SCL  17  // ALSO used as SI4732 AUDIO ADC pin — do not use for I2C

// --- APA102 RGB LED (SPI) ---
#define PIN_LED_CLK   45  // APA102_CLK
#define PIN_LED_DATA  42  // APA102_DI
#define NUM_LEDS       1  // Single onboard RGB LED

// --- TF / MicroSD Card (SPI) ---
#define PIN_SD_CS     39
#define PIN_SD_SCLK   40
#define PIN_SD_MOSI   41
#define PIN_SD_MISO   38

// --- Grove Connector ---
#define PIN_GROVE_IO44  44
#define PIN_GROVE_IO43  43


// ============================================================
// Audio Configuration
// ============================================================

// ADC path (analog audio from SI4732 on IO17)
// ESP32-S3 ADC1 channel for IO17 = ADC1_CHANNEL_6
// Using ADC continuous mode with DMA for low-overhead capture.
#define AUDIO_ADC_CHANNEL    ADC_CHANNEL_6  // IO17 = ADC1_CH6
#define AUDIO_ADC_UNIT       ADC_UNIT_1
#define AUDIO_SAMPLE_RATE_HZ 12000   // ADC sample rate — 12kHz matches ft8_lib and JS8Call natively
#define AUDIO_ADC_ATTEN      ADC_ATTEN_DB_12  // Full 0-3.3V range
#define AUDIO_ADC_BITWIDTH   ADC_BITWIDTH_12  // 12-bit resolution
#define AUDIO_DMA_BUF_COUNT  8
#define AUDIO_DMA_BUF_LEN    512    // Samples per DMA buffer

// I2S Speaker output sample rate.
// 16 kHz is confirmed to produce audible output on the MAX98357A amp.
// 12 kHz caused the speaker to produce no sound (clock dividers issue).
// ADC (12 kHz) → I2S (16 kHz) requires 4:3 upsampling in AudioCapture.cpp.
#define SPEAKER_SAMPLE_RATE_HZ  16000

// ============================================================
// FFT / Waterfall
// ============================================================
#define FFT_SIZE         512   // Must be power of 2
#define WATERFALL_FPS     10   // Target frames per second
#define WATERFALL_COLS   256   // Frequency bins sent to browser

// ============================================================
// I2S Port Assignments
// Only one I2S port needed: speaker output.
// ADC continuous mode is separate from I2S.
// ============================================================
#define I2S_PORT_SPEAKER   I2S_NUM_1  // Confirmed: factory spk_init() uses I2S_NUM_1
#define I2S_PORT_MIC       I2S_NUM_0  // Confirmed: factory mic_init() uses I2S_NUM_0 (ES7210)

// ============================================================
// FreeRTOS Task Priorities and Stack Sizes
// ============================================================
#define TASK_PRIO_RADIO       5
#define TASK_PRIO_AUDIO       6   // Highest — audio DMA must not be starved
#define TASK_PRIO_DSP         4
#define TASK_PRIO_WEBSTREAM   3
#define TASK_PRIO_DISPLAY     2
#define TASK_PRIO_INPUT       4

#define STACK_RADIO      4096
#define STACK_AUDIO      8192
#define STACK_DSP        8192
#define STACK_WEBSTREAM  8192   // Increased: WaterfallRow (1028 B) on stack + FreeRTOS overhead
#define STACK_DISPLAY    4096
#define STACK_INPUT      2048

// Core assignments
// Core 1: time-critical tasks (audio DMA, radio control, DSP)
// Core 0: networking (WiFi, web server, WebSocket, display)
#define CORE_RADIO   1
#define CORE_WEB     0
