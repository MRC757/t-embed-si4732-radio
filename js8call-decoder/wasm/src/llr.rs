/// Soft log-likelihood ratio (LLR) computation.
///
/// For each of the 58 data symbols (3 bits each → 174 LLRs total) we read
/// the 8-tone power values from the spectrogram and convert them to
/// bit-level LLRs via the approximation from Taylor & Franke (QEX 2020).
///
/// LLR convention: positive = bit is 0, negative = bit is 1.
use crate::types::{COSTAS_TONES, SYNC_POSITIONS};

/// Extract 174 soft LLRs for one candidate.
///
/// `spec`   — full spectrogram [time_slot][freq_bin] (already time-aligned)
/// `f0_bin` — bin of the lowest (tone-0) carrier
/// `t0`     — first symbol slot (frame start)
/// `tone_bins` — bins per tone step (1 for all modes at 12 kHz)
pub fn extract_llrs(
    spec:      &[Vec<f32>],
    f0_bin:    usize,
    t0:        usize,
    tone_bins: usize,
) -> Option<[f32; 174]> {
    let data_positions = crate::types::data_positions();
    let mut llrs = [0.0f32; 174];

    for (sym_idx, &sym_pos) in data_positions.iter().enumerate() {
        let t = t0 + sym_pos;
        if t >= spec.len() {
            return None;
        }
        let row = &spec[t];

        // Read power in each of the 8 tones.
        let mut power = [0.0f32; 8];
        for tone in 0..8 {
            let b = f0_bin + tone * tone_bins;
            if b < row.len() {
                power[tone] = row[b].max(1e-10); // avoid log(0)
            }
        }

        // Normalise so magnitudes sum to 1 (makes LLRs comparable across SNRs).
        let total: f32 = power.iter().sum();
        if total <= 0.0 {
            return None;
        }
        for p in &mut power {
            *p /= total;
        }

        // Gray-coded 8-FSK: tone index i → 3 bits b2 b1 b0 (Gray code).
        // Gray decode: b2 = g2, b1 = g2^g1, b0 = g2^g1^g0 where (g2,g1,g0) = index bits.
        // For each output bit position, LLR = log( P(bit=0) / P(bit=1) ).
        for bit_pos in 0..3 {
            let mut p0 = 0.0f32; // total prob that this bit = 0
            let mut p1 = 0.0f32; // total prob that this bit = 1
            for tone in 0u8..8 {
                let gray = tone ^ (tone >> 1); // tone index to Gray code
                let bit  = (gray >> (2 - bit_pos)) & 1;
                if bit == 0 {
                    p0 += power[tone as usize];
                } else {
                    p1 += power[tone as usize];
                }
            }
            let llr = (p0 / p1.max(1e-10)).ln();
            llrs[sym_idx * 3 + bit_pos] = llr;
        }
    }

    Some(llrs)
}

/// Estimate SNR in dB from the spectrogram column at the carrier frequency.
/// Simple estimator: peak-bin power vs mean of surrounding bins.
pub fn estimate_snr(
    spec:      &[Vec<f32>],
    f0_bin:    usize,
    t0:        usize,
    n_symbols: usize,
    tone_bins: usize,
    n_tones:   usize,
) -> f32 {
    let signal_bins = n_tones * tone_bins;
    let mut sig = 0.0f32;
    let mut noise = 0.0f32;

    for sym_pos in 0..n_symbols {
        let t = t0 + sym_pos;
        if t >= spec.len() {
            break;
        }
        let row = &spec[t];
        for tone in 0..n_tones {
            let b = f0_bin + tone * tone_bins;
            if b < row.len() {
                sig += row[b];
            }
        }
        // Noise estimate from the bins just above the signal band.
        let noise_start = f0_bin + signal_bins;
        for b in noise_start..(noise_start + signal_bins).min(row.len()) {
            noise += row[b];
        }
    }

    let signal_avg = sig   / (n_symbols * n_tones) as f32;
    let noise_avg  = noise / (n_symbols * n_tones) as f32;
    10.0 * (signal_avg / noise_avg.max(1e-10)).log10()
}
