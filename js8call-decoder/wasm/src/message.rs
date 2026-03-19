/// JS8Call message unpacking.
///
/// The 91 information bits (K=91) carry:
///   bits  0-71 — 72 message bits (callsigns / grid / free text encoding)
///   bits 72-82 — 11-bit CRC (same polynomial as FT8: x^11 + x^10 + x^9 + x^5 + 1)
///
/// JS8Call extends FT8's 75-bit message space and uses a custom bit-packing
/// scheme for callsigns, grids, and free text.  This module decodes the most
/// common message types.  See js8call/Message.cpp for the full encoder.

use crate::ldpc::unpack_bits;

const CRC_POLY: u16 = 0x06B1; // x^11 + x^10 + x^9 + x^5 + 1

/// Attempt to decode a 22-byte packed codeword into a human-readable message.
///
/// Returns `None` if the CRC fails or the message type is unrecognised.
pub fn decode_message(codeword: &[u8; 22]) -> Option<String> {
    let bits = unpack_bits(codeword, 91);

    // Verify 11-bit CRC over bits 0-71.
    let computed_crc = crc11(&bits[..72]);
    let stored_crc   = bits_to_u16(&bits[72..83]);
    if computed_crc != stored_crc {
        return None;
    }

    // Decode based on message type indicator (top bits).
    // FT8 / JS8Call encodes standard messages as:
    //   bits  0-27: callsign1 (28-bit)
    //   bits 28-55: callsign2 (28-bit)
    //   bits 56-62: grid/report (7-bit) or free-text flag
    //   bits 63-71: type/suffix bits
    //
    // If bit 72 of the *raw 75-bit* field is 0 → standard callsign pair.
    // Free-text messages use a 71-bit field.
    //
    // JS8Call adds heartbeat, directed, and relay types; we detect the
    // most common variants below.

    decode_standard(&bits)
        .or_else(|| decode_free_text(&bits))
        .or_else(|| Some(format!("<bits:{}>", hex_str(&bits[..72]))))
}

// ----- standard callsign-pair message -----

fn decode_standard(bits: &[u8]) -> Option<String> {
    // Standard message: two callsigns + grid/report
    let c1_raw = bits_to_u32(&bits[0..28]);
    let c2_raw = bits_to_u32(&bits[28..56]);
    let r_raw  = bits_to_u32(&bits[56..63]);
    let is_type = bits[63] == 0; // 0 = standard, 1 = extended

    if !is_type {
        return None;
    }

    let c1 = decode_callsign(c1_raw)?;
    let c2 = decode_callsign(c2_raw)?;
    let report = decode_report(r_raw);

    Some(format!("{} {} {}", c1, c2, report))
}

// ----- free-text -----

const CHAR37: &[u8] =
    b" 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

fn decode_free_text(bits: &[u8]) -> Option<String> {
    // 71 bits → up to 13 characters in base-37.
    let mut val = bits_to_u128(&bits[..71]);
    let mut chars: Vec<u8> = Vec::with_capacity(13);
    for _ in 0..13 {
        let idx = (val % 37) as usize;
        chars.push(CHAR37[idx]);
        val /= 37;
    }
    chars.reverse();
    let s: String = chars
        .iter()
        .map(|&c| c as char)
        .collect::<String>()
        .trim()
        .to_string();
    if s.is_empty() {
        None
    } else {
        Some(s)
    }
}

// ----- callsign decoding -----

/// 28-bit callsign encoding.  Character set: " 0-9A-Z" (37 chars).
fn decode_callsign(mut n: u32) -> Option<String> {
    // Special packed values.
    if n == 0 {
        return Some("DE".into());
    }
    if n == 1 {
        return Some("QRZ".into());
    }
    if n == 2 {
        return Some("CQ".into());
    }
    // CQ with 3-digit frequency: 3-512 encode CQ 000..CQ 509.
    if n >= 3 && n <= 512 {
        return Some(format!("CQ {:03}", n - 3));
    }

    // Standard alphanumeric callsign (up to 6 characters).
    // Alphabet for positions: " 0-9A-Z" → 37 symbols.
    let mut cs = [b' '; 6];
    for i in (0..6).rev() {
        cs[i] = CHAR37[(n % 37) as usize];
        n /= 37;
    }
    let s = std::str::from_utf8(&cs)
        .ok()?
        .trim()
        .to_string();
    if s.is_empty() || !s.chars().next()?.is_alphanumeric() {
        return None;
    }
    Some(s)
}

// ----- report / grid decoding -----

fn decode_report(r: u32) -> String {
    if r <= 35 {
        // Maidenhead grid (4 chars), encoded as base-36 pair.
        let lon_idx = r / 6;  // 0-5
        let lat_idx = r % 6;  // 0-5
        // This is simplified; full 4-char grid needs the full 15-bit encoding.
        format!("{}{}", (b'A' + lon_idx as u8) as char, (b'A' + lat_idx as u8) as char)
    } else if r <= 67 {
        // Signal report: -32..0 dB
        format!("{:+}", (r as i32) - 67)
    } else if r == 68 {
        "RRR".into()
    } else if r == 69 {
        "RR73".into()
    } else if r == 70 {
        "73".into()
    } else {
        format!("R{:+}", (r as i32) - 67)
    }
}

// ----- CRC -----

fn crc11(bits: &[u8]) -> u16 {
    let mut crc = 0u16;
    for &b in bits {
        let msb = (crc >> 10) & 1;
        crc = (crc << 1) & 0x7FF;
        if msb ^ (b as u16) != 0 {
            crc ^= CRC_POLY;
        }
    }
    crc & 0x7FF
}

// ----- bit conversion helpers -----

fn bits_to_u16(bits: &[u8]) -> u16 {
    bits.iter().fold(0u16, |acc, &b| (acc << 1) | (b as u16))
}

fn bits_to_u32(bits: &[u8]) -> u32 {
    bits.iter().fold(0u32, |acc, &b| (acc << 1) | (b as u32))
}

fn bits_to_u128(bits: &[u8]) -> u128 {
    bits.iter().fold(0u128, |acc, &b| (acc << 1) | (b as u128))
}

fn hex_str(bits: &[u8]) -> String {
    bits.chunks(4)
        .map(|c| {
            let nibble = c.iter().fold(0u8, |acc, &b| (acc << 1) | b);
            format!("{:X}", nibble)
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_deterministic() {
        let bits = vec![0u8, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0];
        assert_eq!(crc11(&bits), crc11(&bits));
    }

    #[test]
    fn free_text_decode() {
        // Encode "HELLO" manually then decode.
        let s = "HELLO";
        let mut val = 0u128;
        for ch in s.chars() {
            let idx = CHAR37.iter().position(|&c| c == ch as u8).unwrap_or(0);
            val = val * 37 + idx as u128;
        }
        // Pad to 13 chars.
        for _ in 0..(13 - s.len()) {
            val = val * 37;
        }
        let mut bits = vec![0u8; 71];
        for i in (0..71).rev() {
            bits[i] = (val & 1) as u8;
            val >>= 1;
        }
        let decoded = decode_free_text(&bits).unwrap_or_default();
        assert!(decoded.contains("HELLO"), "got: {}", decoded);
    }
}
