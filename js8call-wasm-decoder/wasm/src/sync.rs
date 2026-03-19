/// Costas-array synchronisation and candidate detection.
///
/// Strategy
/// --------
/// For every (time_offset, freq_bin) grid point, compute a correlation
/// score by summing the spectrogram magnitudes at the bins expected by
/// the three Costas arrays embedded in the frame.  Keep the top-N
/// candidates by score and pass them to the LDPC stage.
use crate::types::{Candidate, COSTAS_TONES, SYNC_POSITIONS, SpeedMode};

/// One entry in the candidate list before LDPC.
#[derive(Clone, Debug)]
pub struct SyncCandidate {
    pub freq_bin:    usize,   // bin of the lowest (tone-0) carrier
    pub time_offset: i32,     // symbol-slot offset (can be negative)
    pub sync_score:  f32,
}

/// Search the spectrogram for Costas-array matches.
///
/// Parameters
/// ----------
/// `spec`          — 2-D spectrogram [time_slot][freq_bin]
/// `mode`          — speed mode (determines how many bins per tone step)
/// `freq_min_hz`   — lower edge of search band
/// `freq_max_hz`   — upper edge of search band
/// `sample_rate`   — nominal sample rate (12 000 Hz)
/// `max_candidates`— how many top hits to return
pub fn search_candidates(
    spec: &[Vec<f32>],
    mode: SpeedMode,
    freq_min_hz: f32,
    freq_max_hz: f32,
    sample_rate: f32,
    max_candidates: usize,
) -> Vec<SyncCandidate> {
    let fft_size      = mode.fft_size();
    let bin_hz        = sample_rate / fft_size as f32;
    let n_tones       = 8usize;
    let tone_bins     = 1usize; // one FFT bin per tone (tone_spacing == bin_hz)

    let min_bin = (freq_min_hz / bin_hz).floor() as usize;
    let max_bin = (freq_max_hz / bin_hz).ceil() as usize;

    // Search span: each tone occupies 1 bin; 8 tones occupy 8 bins.
    // The maximum start bin is max_bin - (n_tones - 1) * tone_bins.
    let span_bins = (n_tones - 1) * tone_bins;
    if max_bin < min_bin + span_bins {
        return vec![];
    }

    let n_time_slots = spec.len();
    // A full frame is 79 symbols.  Search time offsets so the entire
    // 79-slot frame fits inside `spec`.
    let frame_slots = 79usize;
    if n_time_slots < frame_slots {
        return vec![];
    }

    let mut hits: Vec<SyncCandidate> = Vec::new();

    for t0 in 0..=(n_time_slots - frame_slots) {
        for f0 in min_bin..=(max_bin - span_bins) {
            let score = costas_score(spec, t0, f0, tone_bins);
            hits.push(SyncCandidate {
                freq_bin:    f0,
                time_offset: t0 as i32,
                sync_score:  score,
            });
        }
    }

    // Sort descending by score, keep top-N.
    hits.sort_by(|a, b| b.sync_score.partial_cmp(&a.sync_score).unwrap_or(std::cmp::Ordering::Equal));
    hits.truncate(max_candidates);
    hits
}

/// Compute Costas correlation score for a given (time, freq) origin.
///
/// Sums the spectrogram power at the expected Costas bins across all
/// three arrays, then subtracts the power of the off-sequence bins to
/// create a contrast score.
fn costas_score(
    spec:      &[Vec<f32>],
    t0:        usize,
    f0:        usize,
    tone_bins: usize,
) -> f32 {
    let mut score = 0.0f32;

    for &positions in &SYNC_POSITIONS {
        for (k, &sym_pos) in positions.iter().enumerate() {
            let t = t0 + sym_pos;
            if t >= spec.len() {
                return f32::NEG_INFINITY;
            }
            let row = &spec[t];
            let expected_bin = f0 + COSTAS_TONES[k] as usize * tone_bins;
            if expected_bin >= row.len() {
                return f32::NEG_INFINITY;
            }

            // Add expected-tone power.
            score += row[expected_bin];

            // Subtract mean of the other 7 bins in the 8-tone window
            // (soft "contrast" rather than just peak power).
            let mut noise_sum = 0.0f32;
            for tone in 0..8 {
                if tone != COSTAS_TONES[k] as usize {
                    let b = f0 + tone * tone_bins;
                    if b < row.len() {
                        noise_sum += row[b];
                    }
                }
            }
            score -= noise_sum / 7.0;
        }
    }

    score
}

/// Convert a `SyncCandidate` into a `Candidate` stub for the LDPC stage.
pub fn candidate_from_sync(sc: &SyncCandidate, fft_size: usize, sample_rate: f32) -> Candidate {
    let bin_hz = sample_rate / fft_size as f32;
    Candidate {
        freq_hz:     sc.freq_bin as f32 * bin_hz,
        time_offset: sc.time_offset,
        sync_score:  sc.sync_score,
        codeword:    [0u8; 22],
        ldpc_ok:     false,
        message:     None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_costas_spec(t0: usize, f0: usize, n_time: usize, n_freq: usize) -> Vec<Vec<f32>> {
        let mut spec = vec![vec![0.0f32; n_freq]; n_time];
        for positions in &SYNC_POSITIONS {
            for (k, &sym_pos) in positions.iter().enumerate() {
                let t = t0 + sym_pos;
                let b = f0 + COSTAS_TONES[k] as usize;
                if t < n_time && b < n_freq {
                    spec[t][b] = 100.0;
                }
            }
        }
        spec
    }

    #[test]
    fn detects_planted_costas() {
        let t0 = 2usize;
        let f0 = 30usize;
        let spec = make_costas_spec(t0, f0, 85, 200);
        let score = costas_score(&spec, t0, f0, 1);
        assert!(score > 0.0, "expected positive score, got {}", score);
    }
}
