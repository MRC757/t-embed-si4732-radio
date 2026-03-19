/// JS8Call / FT8 speed modes.
/// Each mode changes symbol duration and FFT size at a fixed 12 kHz sample rate.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SpeedMode {
    Slow,    // 320 ms / symbol, 3840 FFT, 3.125 Hz tone spacing
    Normal,  // 160 ms / symbol, 1920 FFT, 6.25  Hz tone spacing
    Fast,    //  80 ms / symbol,  960 FFT, 12.5  Hz tone spacing
    Turbo,   //  40 ms / symbol,  480 FFT, 25.0  Hz tone spacing
}

impl SpeedMode {
    /// FFT size (= samples per symbol at 12 kHz).
    pub fn fft_size(self) -> usize {
        match self {
            SpeedMode::Slow   => 3840,
            SpeedMode::Normal => 1920,
            SpeedMode::Fast   =>  960,
            SpeedMode::Turbo  =>  480,
        }
    }

    /// Tone spacing in Hz (= 1 / symbol_duration).
    pub fn tone_spacing_hz(self) -> f32 {
        12000.0 / self.fft_size() as f32
    }

    /// Nominal number of samples per full 79-symbol transmission.
    pub fn frame_samples(self) -> usize {
        79 * self.fft_size()
    }

    pub fn from_u8(v: u8) -> Self {
        match v {
            0 => SpeedMode::Slow,
            1 => SpeedMode::Normal,
            2 => SpeedMode::Fast,
            3 => SpeedMode::Turbo,
            _ => SpeedMode::Normal,
        }
    }
}

/// A decoded candidate before message parsing.
#[derive(Clone, Debug)]
pub struct Candidate {
    /// Frequency offset of the lowest tone (Hz).
    pub freq_hz: f32,
    /// Time offset in samples from the start of the search window.
    pub time_offset: i32,
    /// Costas correlation score (higher = better sync).
    pub sync_score: f32,
    /// Raw 174-bit LDPC codeword after decoding (bit 0 = MSB of byte 0).
    pub codeword: [u8; 22], // 174 bits packed into ceil(174/8)=22 bytes
    /// Whether LDPC converged (syndrome = 0).
    pub ldpc_ok: bool,
    /// Decoded message string (populated after unpacking).
    pub message: Option<String>,
}

/// Everything the JS side needs to know about one decode.
#[derive(Clone, Debug)]
pub struct DecodeResult {
    pub freq_hz: f32,
    pub snr_db:  f32,
    pub message: String,
}

// ----- frame geometry -----

/// Positions of the 7 sync symbols within the 79-symbol frame.
/// Costas arrays appear at symbols 0-6, 36-42, 72-78.
pub const SYNC_POSITIONS: [[usize; 7]; 3] = [
    [0, 1, 2, 3, 4, 5, 6],
    [36, 37, 38, 39, 40, 41, 42],
    [72, 73, 74, 75, 76, 77, 78],
];

/// The Costas array tone sequence (0-indexed tones 0-7).
/// Same as FT8: [2, 5, 6, 0, 4, 1, 3].
pub const COSTAS_TONES: [u8; 7] = [2, 5, 6, 0, 4, 1, 3];

/// Data symbol positions within the frame (the 58 non-sync slots).
pub fn data_positions() -> [usize; 58] {
    let sync_set: std::collections::HashSet<usize> = SYNC_POSITIONS
        .iter()
        .flatten()
        .copied()
        .collect();
    let mut out = [0usize; 58];
    let mut idx = 0;
    for sym in 0..79 {
        if !sync_set.contains(&sym) {
            out[idx] = sym;
            idx += 1;
        }
    }
    assert_eq!(idx, 58);
    out
}
