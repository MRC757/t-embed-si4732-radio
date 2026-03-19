/// Sliding-window FFT spectrogram.
///
/// Produces a 2-D array [time_slot × freq_bin] of squared magnitudes
/// that the sync detector consumes. One column per symbol period.
use rustfft::{num_complex::Complex, FftPlanner};

pub struct Spectrogram {
    fft_size: usize,
    /// Precomputed Hann window coefficients.
    window:   Vec<f32>,
    /// FFT plan (re-used across calls).
    planner:  FftPlanner<f32>,
}

impl Spectrogram {
    pub fn new(fft_size: usize) -> Self {
        let window = hann_window(fft_size);
        Self {
            fft_size,
            window,
            planner: FftPlanner::new(),
        }
    }

    /// Compute one column of the spectrogram for a single symbol-length slice.
    ///
    /// `samples` must have length >= `fft_size`.  Returns the lower half of
    /// the magnitude spectrum (DC … Nyquist), length = fft_size / 2 + 1.
    pub fn process_symbol(&mut self, samples: &[f32]) -> Vec<f32> {
        assert!(samples.len() >= self.fft_size);
        let fft = self.planner.plan_fft_forward(self.fft_size);

        let mut buf: Vec<Complex<f32>> = samples[..self.fft_size]
            .iter()
            .zip(self.window.iter())
            .map(|(&s, &w)| Complex { re: s * w, im: 0.0 })
            .collect();

        fft.process(&mut buf);

        // Return squared magnitudes for the positive-frequency half.
        let half = self.fft_size / 2 + 1;
        buf[..half]
            .iter()
            .map(|c| c.norm_sqr())
            .collect()
    }

    /// Process a full capture window (`samples.len()` = n_symbols × fft_size)
    /// and return the 2-D spectrogram [n_symbols][fft_size/2+1].
    pub fn process_frame(&mut self, samples: &[f32]) -> Vec<Vec<f32>> {
        let stride = self.fft_size;
        let n_slots = samples.len() / stride;
        (0..n_slots)
            .map(|i| self.process_symbol(&samples[i * stride..]))
            .collect()
    }

    /// Return the FFT bin index corresponding to a given frequency.
    pub fn freq_to_bin(&self, freq_hz: f32, sample_rate: f32) -> usize {
        let bin_width = sample_rate / self.fft_size as f32;
        (freq_hz / bin_width).round() as usize
    }

    pub fn fft_size(&self) -> usize {
        self.fft_size
    }
}

fn hann_window(n: usize) -> Vec<f32> {
    use std::f32::consts::PI;
    (0..n)
        .map(|i| 0.5 * (1.0 - (2.0 * PI * i as f32 / (n as f32 - 1.0)).cos()))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spectrogram_shape() {
        let mut sg = Spectrogram::new(1920);
        let samples = vec![0.0f32; 1920];
        let col = sg.process_symbol(&samples);
        assert_eq!(col.len(), 961); // 1920/2 + 1
    }

    #[test]
    fn single_tone_detected() {
        use std::f32::consts::PI;
        let fft_size = 1920;
        let sr = 12000.0f32;
        let freq = 1000.0f32;
        let samples: Vec<f32> = (0..fft_size)
            .map(|i| (2.0 * PI * freq * i as f32 / sr).sin())
            .collect();
        let mut sg = Spectrogram::new(fft_size);
        let col = sg.process_symbol(&samples);
        let expected_bin = (freq / (sr / fft_size as f32)).round() as usize;
        let peak_bin = col
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.partial_cmp(b.1).unwrap())
            .unwrap()
            .0;
        assert_eq!(peak_bin, expected_bin);
    }
}
