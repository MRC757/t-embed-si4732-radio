#pragma once
// ============================================================
// BandConfig.h — Band and mode definitions for SI4732
// All frequency values in kHz unless noted.
//
// SSB is demodulated in SOFTWARE (SoftSSBDemod) — no patch.
// See src/dsp/SoftSSBDemod.h for full explanation.
// ============================================================
#include <Arduino.h>

// ============================================================
// Demodulation modes
// ============================================================
enum class DemodMode : uint8_t {
    FM   = 0,  // FM broadcast 64–108 MHz
    AM   = 1,  // AM broadcast MW 520–1710 kHz
    LSB  = 2,  // Lower Sideband — software product detector
    USB  = 3,  // Upper Sideband — software product detector
    CW   = 4,  // CW — software product detector, 700 Hz BFO
    LW   = 5,  // Longwave AM 153–279 kHz
    SW   = 6,  // Shortwave AM 2.3–26.1 MHz
};

inline const char* demodModeStr(DemodMode m) {
    switch(m) {
        case DemodMode::FM:  return "FM";
        case DemodMode::AM:  return "AM";
        case DemodMode::LSB: return "LSB";
        case DemodMode::USB: return "USB";
        case DemodMode::CW:  return "CW";
        case DemodMode::LW:  return "LW";
        case DemodMode::SW:  return "SW";
        default:             return "??";
    }
}

inline bool isSoftSSBMode(DemodMode m) {
    return m == DemodMode::LSB || m == DemodMode::USB || m == DemodMode::CW;
}

// ============================================================
// AM bandwidth filter options for SI4732
// Used in AM and software-SSB modes.
// ============================================================
// Index → kHz:  0=6, 1=4, 2=3, 3=2, 4=1, 5=1.8, 6=2.5
// For software SSB, index 2 (3 kHz) is the best tradeoff:
//   wide enough to pass voice, narrow enough to reduce adjacent
//   channel interference before the product detector.
// For CW, index 4 (1 kHz) provides better selectivity.
enum class AMBandwidth : uint8_t {
    BW_6kHz   = 0,
    BW_4kHz   = 1,
    BW_3kHz   = 2,  // ← software SSB default
    BW_2kHz   = 3,
    BW_1kHz   = 4,  // ← CW default
    BW_1p8kHz = 5,
    BW_2p5kHz = 6,
};

// ============================================================
// Band definition
// ============================================================
struct Band {
    const char* name;
    uint32_t    freqMin;        // kHz
    uint32_t    freqMax;        // kHz
    uint32_t    freqDefault;    // kHz
    uint16_t    stepKHz;        // coarse encoder step
    DemodMode   mode;
    uint8_t     amBwIdx;        // AM bandwidth filter index
    // Software SSB parameters
    int         defaultBfoHz;   // BFO frequency for product detector
    int         lsbTuneOffsetKHz; // LSB: SI4732 tuned (freqKHz + offset) kHz
                                   // USB/CW: 0 (tune exactly to dial freq)
};

// ============================================================
// Software-SSB tuning notes
// ============================================================
// The SI4732 is always in AM mode for SSB reception.
// The chip's AM bandwidth filter limits the passband to amBwIdx
// kHz around the tuned frequency.
//
// USB: tune SI4732 to the DIAL frequency.
//   The AM passband covers 0–3 kHz above the carrier.
//   BFO at 1500 Hz shifts the voice passband (300–2700 Hz)
//   to centre at 0 Hz after the LPF.
//
// LSB: tune SI4732 to (DIAL + lsbTuneOffsetKHz).
//   LSB voice is BELOW the carrier, so by tuning the chip
//   2 kHz above the dial frequency, the LSB voice (which
//   appears as 300–2700 Hz below the carrier, i.e. at
//   −2700 to −300 Hz) shifts into the AM passband
//   at approximately 300–2700 Hz relative to chip centre.
//   BFO at 1500 Hz then recovers the voice.
//
// The result is that USB and LSB use the same BFO frequency;
// only the SI4732 chip tuning differs.
// ============================================================
static constexpr int DEFAULT_USB_BFO_HZ  = 1500;
static constexpr int DEFAULT_LSB_BFO_HZ  = 1500;
static constexpr int DEFAULT_CW_BFO_HZ   = 700;
static constexpr int LSB_TUNE_OFFSET_KHZ = 3; // shift chip up by 3 kHz for LSB

// ============================================================
// Band table
// ============================================================
static const Band BAND_TABLE[] = {
    //  Name        Min    Max    Default  Step  Mode            BWIdx  BFO   LSBOff
    { "FM",        6400, 10800,  10390,   10,  DemodMode::FM,   0,     0,    0 },
    { "LW",         153,   279,    198,    1,  DemodMode::LW,   0,     0,    0 },
    { "MW",         520,  1710,    720,    9,  DemodMode::AM,   0,     0,    0 },
    { "SW 120m",   2300,  2495,   2400,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 90m",    3200,  3400,   3300,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 75m",    3900,  4000,   3975,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 60m",    4750,  5060,   5000,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 49m",    5900,  6200,   6000,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 41m",    7200,  7450,   7200,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 31m",    9400,  9900,   9600,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 25m",   11600, 12100,  11800,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 22m",   13570, 13870,  13700,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 19m",   15100, 15800,  15400,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 16m",   17480, 17900,  17700,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 13m",   21450, 21850,  21600,    5,  DemodMode::SW,   0,     0,    0 },
    { "SW 11m",   25600, 26100,  25800,    5,  DemodMode::SW,   0,     0,    0 },
    // CB — 40 channel AM, 10 kHz spacing; ch 19 (27185 kHz) is calling/truckers
    { "CB 11m",   26965, 27405,  27185,   10,  DemodMode::AM,   1,     0,    0 },
    // Marine HF — USB voice, ITU calling channels
    { "Marine 4MHz",  4000,  4438,   4125,   1,  DemodMode::USB,  2,  DEFAULT_USB_BFO_HZ, 0 },
    { "Marine 8MHz",  8195,  8815,   8291,   1,  DemodMode::USB,  2,  DEFAULT_USB_BFO_HZ, 0 },
    { "Marine16MHz", 16360, 17410,  16420,   1,  DemodMode::USB,  2,  DEFAULT_USB_BFO_HZ, 0 },
    // WWV/WWVH NIST time signal stations (AM, spot frequencies)
    { "WWV 2.5MHz",  2498,  2502,   2500,   1,  DemodMode::AM,   0,     0,    0 },
    { "WWV 5MHz",    4998,  5002,   5000,   1,  DemodMode::AM,   0,     0,    0 },
    { "WWV 10MHz",   9998, 10002,  10000,   1,  DemodMode::AM,   0,     0,    0 },
    { "WWV 15MHz",  14998, 15002,  15000,   1,  DemodMode::AM,   0,     0,    0 },
    { "WWV 20MHz",  19998, 20002,  20000,   1,  DemodMode::AM,   0,     0,    0 },
    // HAM bands — software SSB
    { "HAM 160m",  1800,  2000,   1900,    1,  DemodMode::LSB,  2,  DEFAULT_LSB_BFO_HZ, LSB_TUNE_OFFSET_KHZ },
    { "HAM 80m",   3500,  4000,   3750,    1,  DemodMode::LSB,  2,  DEFAULT_LSB_BFO_HZ, LSB_TUNE_OFFSET_KHZ },
    { "HAM 60m",   5351,  5367,   5360,    1,  DemodMode::USB,  2,  DEFAULT_USB_BFO_HZ, 0 },
    { "HAM 40m",   7000,  7300,   7100,    1,  DemodMode::LSB,  2,  DEFAULT_LSB_BFO_HZ, LSB_TUNE_OFFSET_KHZ },
    { "HAM 30m",  10100, 10150,  10125,    1,  DemodMode::USB,  2,  DEFAULT_USB_BFO_HZ, 0 },
    { "HAM 20m",  14000, 14350,  14074,    1,  DemodMode::USB,  2,  DEFAULT_USB_BFO_HZ, 0 },
    { "HAM 17m",  18068, 18168,  18100,    1,  DemodMode::USB,  2,  DEFAULT_USB_BFO_HZ, 0 },
    { "HAM 15m",  21000, 21450,  21074,    1,  DemodMode::USB,  2,  DEFAULT_USB_BFO_HZ, 0 },
    { "HAM 12m",  24890, 24990,  24915,    1,  DemodMode::USB,  2,  DEFAULT_USB_BFO_HZ, 0 },
    { "HAM 10m",  28000, 29700,  28074,    1,  DemodMode::USB,  2,  DEFAULT_USB_BFO_HZ, 0 },
    { "HAM CW",    3500, 30000,  14025,    1,  DemodMode::CW,   4,  DEFAULT_CW_BFO_HZ,  0 },
};

static constexpr int BAND_COUNT = sizeof(BAND_TABLE) / sizeof(BAND_TABLE[0]);

// ── FT8 / WSPR quick-tune frequencies ─────────────────────────
struct DigitalFreq { const char* band; uint32_t freqKHz; };

static const DigitalFreq FT8_FREQS[] = {
    { "80m",  3573  }, { "60m",  5357  }, { "40m",  7074  },
    { "30m", 10136  }, { "20m", 14074  }, { "17m", 18100  },
    { "15m", 21074  }, { "12m", 24915  }, { "10m", 28074  },
};
static constexpr int FT8_FREQ_COUNT = sizeof(FT8_FREQS)/sizeof(FT8_FREQS[0]);
