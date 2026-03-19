mod decimate;
mod ldpc;
mod llr;
mod message;
mod spectrogram;
mod sync;
mod types;

pub use decimate::Decimator;

use types::SpeedMode;
use wasm_bindgen::prelude::*;

// ---------------------------------------------------------------------------
// Public JS-facing API
// ---------------------------------------------------------------------------

/// A decoded message returned to JavaScript.
#[wasm_bindgen]
pub struct DecodeResult {
    pub freq_hz: f32,
    pub snr_db:  f32,
    message:     String,
}

#[wasm_bindgen]
impl DecodeResult {
    #[wasm_bindgen(getter)]
    pub fn message(&self) -> String {
        self.message.clone()
    }
}

// ---------------------------------------------------------------------------

/// Stateful decoder handle exposed to JS.
///
/// Typical call sequence (per frame window):
///   1. `decoder.push_samples(chunk)`   — feed raw PCM
///   2. `decoder.run_decode()`          — trigger full decode
///   3. `decoder.take_results()`        — retrieve decoded messages
#[wasm_bindgen]
pub struct Js8Decoder {
    mode:        SpeedMode,
    sample_rate: f32,
    /// Ring of accumulated PCM samples (f32, mono, 12 kHz).
    buffer:      Vec<f32>,
    /// Minimum samples required before attempting a decode.
    min_samples: usize,
    /// Search band limits (Hz).
    freq_min:    f32,
    freq_max:    f32,
    /// Maximum candidates passed to LDPC.
    max_cands:   usize,
    /// Pending results waiting to be retrieved.
    results:     Vec<DecodeResult>,
}

#[wasm_bindgen]
impl Js8Decoder {
    /// Create a decoder.
    ///
    /// `mode_id`   — 0=Slow, 1=Normal, 2=Fast, 3=Turbo
    /// `freq_min`  — lower search edge in Hz (e.g. 200.0)
    /// `freq_max`  — upper search edge in Hz (e.g. 3000.0)
    #[wasm_bindgen(constructor)]
    pub fn new(mode_id: u8, freq_min: f32, freq_max: f32) -> Self {
        console_error_panic_hook_init();
        let mode = SpeedMode::from_u8(mode_id);
        let sample_rate = 12_000.0f32;
        // Buffer enough for two full frames so we catch transmissions that
        // straddle a slot boundary.
        let min_samples = mode.frame_samples() * 2;
        Self {
            mode,
            sample_rate,
            buffer: Vec::with_capacity(min_samples + 4096),
            min_samples,
            freq_min,
            freq_max,
            max_cands: 20,
            results: Vec::new(),
        }
    }

    /// Append raw PCM samples (f32, mono, must be at 12 kHz).
    pub fn push_samples(&mut self, samples: &[f32]) {
        self.buffer.extend_from_slice(samples);
        // Trim old data: keep 1.5× one frame worth of look-back.
        let keep = self.mode.frame_samples() + self.mode.frame_samples() / 2;
        if self.buffer.len() > self.min_samples + keep {
            let drain = self.buffer.len() - (self.min_samples + keep);
            self.buffer.drain(0..drain);
        }
    }

    /// Run a full decode pass over the accumulated buffer.
    /// Call this once per slot boundary (every `mode.frame_samples()` samples).
    pub fn run_decode(&mut self) {
        if self.buffer.len() < self.min_samples {
            return;
        }

        // 1. Build spectrogram.
        let mut sg = spectrogram::Spectrogram::new(self.mode.fft_size());
        let spec = sg.process_frame(&self.buffer);

        // 2. Costas sync search.
        let candidates = sync::search_candidates(
            &spec,
            self.mode,
            self.freq_min,
            self.freq_max,
            self.sample_rate,
            self.max_cands,
        );

        // 3. For each sync candidate: extract LLRs → LDPC → unpack.
        let tone_bins = 1usize; // 1 bin per tone at 12 kHz
        for sc in &candidates {
            let t0 = sc.time_offset as usize;
            let f0 = sc.freq_bin;

            let llrs = match llr::extract_llrs(&spec, f0, t0, tone_bins) {
                Some(l) => l,
                None    => continue,
            };

            let codeword = match ldpc::decode(&llrs) {
                Some(cw) => cw,
                None     => continue,
            };

            if let Some(msg) = message::decode_message(&codeword) {
                let snr = llr::estimate_snr(
                    &spec, f0, t0, 79, tone_bins, 8,
                );
                let freq_hz = f0 as f32 * (self.sample_rate / self.mode.fft_size() as f32);
                self.results.push(DecodeResult { freq_hz, snr_db: snr, message: msg });
            }
        }
    }

    /// Return and clear all pending decode results.
    pub fn take_results(&mut self) -> js_sys::Array {
        let arr = js_sys::Array::new();
        for r in self.results.drain(..) {
            let obj = js_sys::Object::new();
            js_sys::Reflect::set(&obj, &"freq_hz".into(),  &r.freq_hz.into()).unwrap();
            js_sys::Reflect::set(&obj, &"snr_db".into(),   &r.snr_db.into()).unwrap();
            js_sys::Reflect::set(&obj, &"message".into(), &r.message.into()).unwrap();
            arr.push(&obj);
        }
        arr
    }

    /// Returns the number of samples currently buffered.
    pub fn buffered_samples(&self) -> u32 {
        self.buffer.len() as u32
    }

    /// Clear the sample buffer (e.g. on slot boundary reset).
    pub fn reset_buffer(&mut self) {
        self.buffer.clear();
    }

    /// Change speed mode at runtime.
    pub fn set_mode(&mut self, mode_id: u8) {
        self.mode = SpeedMode::from_u8(mode_id);
        self.min_samples = self.mode.frame_samples() * 2;
        self.buffer.clear();
    }
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

fn console_error_panic_hook_init() {
    #[cfg(feature = "console_error_panic_hook")]
    console_error_panic_hook::set_once();
}

/// Expose build-time version string for debugging.
#[wasm_bindgen]
pub fn version() -> String {
    format!(
        "js8call-wasm {} (rustc {})",
        env!("CARGO_PKG_VERSION"),
        option_env!("RUSTC_VERSION").unwrap_or("unknown"),
    )
}
