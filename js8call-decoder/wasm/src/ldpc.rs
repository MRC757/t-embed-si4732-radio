/// LDPC belief-propagation decoder — FT8/JS8Call (174, 91) code.
///
/// ⚠️  BEFORE BUILDING: run `python3 tools/gen_ldpc_matrix.py` to generate
///     `wasm/src/ldpc_matrix.rs` with the correct parity-check matrix pulled
///     directly from ft8lib.
///
/// Algorithm: sum-product (log-domain belief propagation).
/// Reference: Taylor & Franke, QEX Nov/Dec 2020.

// Include the generated matrix.  Run gen_ldpc_matrix.py to create it.
include!("ldpc_matrix.rs");

/// Run belief-propagation LDPC decoding.
///
/// `llrs` — 174 soft LLR values (positive = bit is 0, negative = bit is 1).
/// Returns `Some(packed_codeword)` when syndrome clears, `None` otherwise.
pub fn decode(llrs: &[f32; 174]) -> Option<[u8; 22]> {
    // V→C messages initialised to channel LLR.
    let mut v2c = [[0.0f32; 3]; N];
    for i in 0..N {
        v2c[i] = [llrs[i]; 3];
    }

    // C→V messages (max check degree = 7).
    let mut c2v = vec![[0.0f32; 7]; M];

    for _iter in 0..MAX_ITER {
        // ---- check-node update (sum-product, log-domain tanh rule) ----
        for j in 0..M {
            for (ej, &vi_u8) in MN[j].iter().enumerate() {
                if vi_u8 == 255 { break; }
                let vi  = vi_u8 as usize;
                let evi = nm_edge_idx(vi, j);

                let mut prod_sign = 1.0f32;
                let mut prod_log  = 0.0f32;

                for (ek, &vk_u8) in MN[j].iter().enumerate() {
                    if vk_u8 == 255 { break; }
                    if ek == ej { continue; }
                    let vk  = vk_u8 as usize;
                    let evk = nm_edge_idx(vk, j);
                    let x   = v2c[vk][evk] * 0.5;
                    let t   = x.tanh();
                    prod_sign *= t.signum();
                    prod_log  += (t.abs() + 1e-10).ln();
                }

                let magnitude = 2.0 * prod_log.exp().min(1.0 - 1e-7).atanh();
                c2v[j][ej]   = prod_sign * magnitude;
            }
        }

        // ---- variable-node update ----
        let mut total = [0.0f32; N];
        for i in 0..N {
            total[i] = llrs[i];
            for &jj in NM[i].iter() {
                let j  = jj as usize;
                let ej = mn_edge_idx(i, j);
                total[i] += c2v[j][ej];
            }
        }

        for i in 0..N {
            for (k, &jj) in NM[i].iter().enumerate() {
                let j  = jj as usize;
                let ej = mn_edge_idx(i, j);
                v2c[i][k] = total[i] - c2v[j][ej];
            }
        }

        // ---- hard decision + syndrome ----
        let mut bits = [0u8; N];
        for i in 0..N { bits[i] = if total[i] < 0.0 { 1 } else { 0 }; }
        if syndrome_ok(&bits) { return Some(pack_bits(&bits)); }
    }

    None
}

#[inline]
fn nm_edge_idx(vi: usize, j: usize) -> usize {
    NM[vi].iter().position(|&c| c as usize == j)
        .unwrap_or_else(|| panic!("nm_edge_idx: check {} not in NM[{}]", j, vi))
}

#[inline]
fn mn_edge_idx(vi: usize, j: usize) -> usize {
    MN[j].iter().position(|&v| v as usize == vi)
        .unwrap_or_else(|| panic!("mn_edge_idx: variable {} not in MN[{}]", vi, j))
}

fn syndrome_ok(bits: &[u8; N]) -> bool {
    for j in 0..M {
        let mut s = 0u8;
        for &vi_u8 in MN[j].iter() {
            if vi_u8 == 255 { break; }
            s ^= bits[vi_u8 as usize];
        }
        if s != 0 { return false; }
    }
    true
}

fn pack_bits(bits: &[u8; N]) -> [u8; 22] {
    let mut out = [0u8; 22];
    for i in 0..N {
        if bits[i] != 0 { out[i / 8] |= 1 << (7 - (i % 8)); }
    }
    out
}

pub fn unpack_bits(bytes: &[u8; 22], len: usize) -> Vec<u8> {
    (0..len).map(|i| (bytes[i / 8] >> (7 - (i % 8))) & 1).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pack_unpack_roundtrip() {
        let bits_in: [u8; 174] = std::array::from_fn(|i| (i % 2) as u8);
        let packed   = pack_bits(&bits_in);
        let bits_out = unpack_bits(&packed, 174);
        assert_eq!(&bits_in[..], &bits_out[..]);
    }

    #[test]
    fn syndrome_all_zero() {
        assert!(syndrome_ok(&[0u8; N]));
    }

    #[test]
    fn decode_strong_zeros() {
        let llrs = [10.0f32; 174];
        assert!(decode(&llrs).is_some(), "strong all-zero LLRs must decode");
    }

    #[test]
    fn nm_mn_consistency() {
        for i in 0..N {
            for &j_u8 in NM[i].iter() {
                let j = j_u8 as usize;
                assert!(
                    MN[j].iter().any(|&v| v as usize == i),
                    "NM[{i}] lists check {j} but MN[{j}] does not list variable {i}"
                );
            }
        }
    }
}
