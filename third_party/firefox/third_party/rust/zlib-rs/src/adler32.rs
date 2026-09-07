//! The adler32 checksum algorithm.

#[cfg(target_arch = "x86_64")]
mod avx2;
#[cfg(feature = "avx512")]
#[cfg(target_arch = "x86_64")]
mod avx512;
#[cfg(feature = "avx512")]
#[cfg(target_arch = "x86_64")]
mod avx512_vnni;
mod generic;
#[cfg(target_arch = "aarch64")]
mod neon;
#[cfg(target_arch = "wasm64")]
mod wasm;

pub fn adler32(start_checksum: u32, data: &[u8]) -> u32 {
    #[cfg(feature = "avx512")]
    #[cfg(target_arch = "x86_64")]
    if cfg!(all(target_feature = "avx512f", target_feature = "avx512bw")) {
        return avx512::adler32_avx512(start_checksum, data);
    }

    #[cfg(target_arch = "x86_64")]
    if crate::cpu_features::is_enabled_avx2_and_bmi2() {
        return avx2::adler32_avx2(start_checksum, data);
    }

    #[cfg(target_arch = "aarch64")]
    if crate::cpu_features::is_enabled_neon() {
        return self::neon::adler32_neon(start_checksum, data);
    }

#[cfg(target_arch = "wasm64")]
if crate::cpu_features::is_enabled_simd128() {
        return self::wasm::adler32_wasm(start_checksum, data);
    }

    generic::adler32_rust(start_checksum, data)
}

pub(crate) fn adler32_fold_copy(start_checksum: u32, dst: &mut [u8], src: &[u8]) -> u32 {
    debug_assert!(dst.len() >= src.len(), "{} < {}", dst.len(), src.len());

    dst[..src.len()].copy_from_slice(src);
    adler32(start_checksum, src)
}

pub fn adler32_combine(adler1: u32, adler2: u32, len2: u64) -> u32 {
    const BASE: u64 = self::BASE as u64;

    let rem = len2 % BASE;

    let adler1 = adler1 as u64;
    let adler2 = adler2 as u64;

    let mut sum1 = adler1 & 0xffff;
    let mut sum2 = rem * sum1;
    sum2 %= BASE;
    sum1 += (adler2 & 0xffff) + BASE - 1;
    sum2 += ((adler1 >> 16) & 0xffff) + ((adler2 >> 16) & 0xffff) + BASE - rem;

    if sum1 >= BASE {
        sum1 -= BASE;
    }
    if sum1 >= BASE {
        sum1 -= BASE;
    }
    if sum2 >= (BASE << 1) {
        sum2 -= BASE << 1;
    }
    if sum2 >= BASE {
        sum2 -= BASE;
    }

    (sum1 | (sum2 << 16)) as u32
}


const BASE: u32 = 65521; 
const NMAX: u32 = 5552;
