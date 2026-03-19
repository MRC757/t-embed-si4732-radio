/// FIR lowpass decimation filter.
///
/// Converts an arbitrary integer sample rate to 12 kHz using a windowed-sinc
/// lowpass filter followed by downsampling.  Preferable to the simple
/// point-decimation in ws-client.ts for signals that are not already
/// band-limited below 6 kHz.
///
/// Common decimation factors:
///   48 000 Hz → 12 000 Hz : factor 4
///   96 000 Hz → 12 000 Hz : factor 8
///  192 000 Hz → 12 000 Hz : factor 16
///
/// Filter design: Kaiser-windowed sinc, cutoff at 0.9 × Nyquist of output
/// (5.4 kHz), enough stop-band to suppress aliasing while preserving the
/// full JS8Call audio band (200 – 3 000 Hz with plenty of headroom).

use wasm_bindgen::prelude::*;

/// Kaiser-windowed sinc FIR coefficients.
///
/// `factor` — integer decimation ratio.
/// `n_taps` — filter length (odd, centred).  Larger = sharper, more latency.
pub fn design_coeffs(factor: usize, n_taps: usize) -> Vec<f32> {
    use std::f64::consts::PI;

    // Cutoff as fraction of input sample rate (= 0.9 / factor of output Nyquist).
    let fc = 0.9 / (2.0 * factor as f64);
    let beta = 6.0_f64; // Kaiser β — controls sidelobe attenuation (~50 dB).
    let m    = n_taps - 1;
    let half = m as f64 / 2.0;

    let i0_beta = bessel_i0(beta);

    let mut coeffs: Vec<f32> = (0..n_taps)
        .map(|n| {
            let x = n as f64 - half;
            // Sinc kernel.
            let sinc = if x == 0.0 {
                2.0 * fc
            } else {
                (2.0 * PI * fc * x).sin() / (PI * x)
            };
            // Kaiser window.
            let arg   = 1.0 - (x / half).powi(2);
            let arg   = if arg < 0.0 { 0.0 } else { arg };
            let win   = bessel_i0(beta * arg.sqrt()) / i0_beta;
            (sinc * win) as f32
        })
        .collect();

    // Normalise so DC gain = 1.
    let sum: f32 = coeffs.iter().sum();
    for c in &mut coeffs { *c /= sum; }

    coeffs
}

/// Zeroth-order modified Bessel function I₀(x) via power series.
fn bessel_i0(x: f64) -> f64 {
    let mut sum  = 1.0;
    let mut term = 1.0;
    for k in 1..=30 {
        term *= (x / 2.0) / k as f64;
        term *= (x / 2.0) / k as f64;
        sum  += term;
        if term < 1e-12 * sum { break; }
    }
    sum
}

// ---------------------------------------------------------------------------
// WASM-facing decimator handle
// ---------------------------------------------------------------------------

/// Stateful FIR decimation filter exposed to JavaScript.
///
/// Maintains a delay line across calls so block boundaries have no artefacts.
#[wasm_bindgen]
pub struct Decimator {
    coeffs:   Vec<f32>,
    delay:    Vec<f32>, // length = n_taps - 1
    factor:   usize,
    /// Phase counter (0 … factor-1); 0 = emit an output sample.
    phase:    usize,
}

#[wasm_bindgen]
impl Decimator {
    /// Create a new decimator.
    ///
    /// `factor`   — integer decimation ratio (e.g. 4 for 48 kHz → 12 kHz)
    /// `n_taps`   — filter length; 127 is a good default for all common factors
    #[wasm_bindgen(constructor)]
    pub fn new(factor: usize, n_taps: usize) -> Self {
        let n_taps  = if n_taps % 2 == 0 { n_taps + 1 } else { n_taps }; // must be odd
        let coeffs  = design_coeffs(factor, n_taps);
        let delay   = vec![0.0f32; n_taps - 1];
        Self { coeffs, delay, factor, phase: 0 }
    }

    /// Process a block of input samples, return decimated output.
    ///
    /// Input: f32 samples at the *source* sample rate.
    /// Output: f32 samples at source_rate / factor (target = 12 kHz).
    pub fn process(&mut self, input: &[f32]) -> Vec<f32> {
        let n_taps = self.coeffs.len();
        let mut out = Vec::with_capacity(input.len() / self.factor + 1);

        for &x in input {
            // Shift the delay line.
            self.delay.rotate_right(1);
            self.delay[0] = x;

            self.phase += 1;
            if self.phase >= self.factor {
                self.phase = 0;
                // Compute convolution at this output sample.
                let mut acc = 0.0f32;
                let n_delay = self.delay.len();
                for (i, &c) in self.coeffs.iter().enumerate() {
                    let s = if i == 0 { x } else { self.delay[i.min(n_delay) - 1] };
                    acc += c * s;
                }
                out.push(acc);
            }
        }

        out
    }

    pub fn factor(&self) -> usize { self.factor }
    pub fn n_taps(&self) -> usize { self.coeffs.len() }
    pub fn group_delay_samples(&self) -> usize { (self.coeffs.len() - 1) / 2 }
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn coeffs_dc_gain_unity() {
        let c = design_coeffs(4, 127);
        let sum: f32 = c.iter().sum();
        assert!((sum - 1.0).abs() < 1e-5, "DC gain = {}", sum);
    }

    #[test]
    fn decimator_output_length() {
        let mut d = Decimator::new(4, 127);
        let input  = vec![0.0f32; 4800]; // 0.4 s at 48 kHz
        let output = d.process(&input);
        // Expect ~ 4800/4 = 1200 output samples (within ±1 for delay).
        assert!((output.len() as i32 - 1200).abs() <= 2);
    }

    #[test]
    fn dc_passthrough() {
        let mut d  = Decimator::new(4, 127);
        let input: Vec<f32> = vec![1.0; 4096];
        let output = d.process(&input);
        // Skip group-delay samples, then DC should be ~1.
        let skip = d.group_delay_samples() / d.factor() + 1;
        let avg: f32 = output[skip..].iter().sum::<f32>() / (output.len() - skip) as f32;
        assert!((avg - 1.0).abs() < 0.01, "DC avg = {}", avg);
    }
}
