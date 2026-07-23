use crate::deflate::core::deflate_flags::{
    TDEFL_FORCE_ALL_RAW_BLOCKS, TDEFL_GREEDY_PARSING_FLAG, TDEFL_RLE_MATCHES,
};

const DEFAULT_CM: u8 = 8;
const DEFAULT_CINFO: u8 = 7 << 4;
const _DEFAULT_FDICT: u8 = 0;
const DEFAULT_CMF: u8 = DEFAULT_CM | DEFAULT_CINFO;
const MIN_CMF: u8 = DEFAULT_CM; 
/// The 16-bit value consisting of CMF and FLG must be divisible by this to be valid.
const FCHECK_DIVISOR: u8 = 31;

/// Generate FCHECK from CMF and FLG (without FCKECH )so that they are correct according to the
/// specification, i.e (CMF*256 + FCHK) % 31 = 0.
/// Returns flg with the FCHKECK bits added (any existing FCHECK bits are ignored).
#[inline]
fn add_fcheck(cmf: u8, flg: u8) -> u8 {
    let rem = ((usize::from(cmf) * 256) + usize::from(flg)) % usize::from(FCHECK_DIVISOR);

    let flg = flg & 0b11100000;

    flg + (FCHECK_DIVISOR - rem as u8)
}

#[inline]
const fn zlib_level_from_flags(flags: u32) -> u8 {
    use crate::deflate::core::NUM_PROBES;

    let num_probes = flags & super::MAX_PROBES_MASK;
    if (flags & TDEFL_GREEDY_PARSING_FLAG != 0) || (flags & TDEFL_RLE_MATCHES != 0) {
        if num_probes <= 1 {
            0
        } else {
            1
        }
    } else if num_probes >= NUM_PROBES[9] as u32 {
        3
    } else {
        2
    }
}

#[inline]
const fn cmf_from_flags(flags: u32) -> u8 {
    if (flags & TDEFL_RLE_MATCHES == 0) && (flags & TDEFL_FORCE_ALL_RAW_BLOCKS == 0) {
        DEFAULT_CMF
    } else {
        MIN_CMF
    }
}

/// Get the zlib header for the level using the default window size and no
/// dictionary.
#[inline]
fn header_from_level(level: u8, flags: u32) -> [u8; 2] {
    let cmf = cmf_from_flags(flags);
    [cmf, add_fcheck(cmf, level << 6)]
}

/// Create a zlib header from the given compression flags.
/// Only level is considered.
#[inline]
pub fn header_from_flags(flags: u32) -> [u8; 2] {
    let level = zlib_level_from_flags(flags);
    header_from_level(level, flags)
}
