use core::arch::x86_64::{
    __m512i, _mm256_add_epi32, _mm256_castsi256_si128, _mm256_extracti128_si256, _mm512_add_epi32,
    _mm512_castsi512_si256, _mm512_extracti64x4_epi64, _mm512_madd_epi16, _mm512_maddubs_epi16,
    _mm512_permutexvar_epi32, _mm512_sad_epu8, _mm512_set1_epi16, _mm512_setr_epi32,
    _mm512_slli_epi32, _mm512_zextsi128_si512, _mm_add_epi32, _mm_cvtsi128_si32, _mm_cvtsi32_si128,
    _mm_shuffle_epi32, _mm_unpackhi_epi64,
};

use crate::adler32::{BASE, NMAX};

const fn __m512i_literal(bytes: [u8; 64]) -> __m512i {
    unsafe { core::mem::transmute(bytes) }
}

const DOT2V: __m512i = __m512i_literal({
    let mut arr = [0; 64];

    let mut i = 64;
    while i > 0 {
        i -= 1;
        arr[i] = (64 - i) as u8;
    }

    arr
});

const ZERO: __m512i = __m512i_literal([0u8; 64]);

pub fn adler32_avx512(adler: u32, src: &[u8]) -> u32 {
    assert!(cfg!(target_feature = "avx512f"));
    assert!(cfg!(target_feature = "avx512bw"));
    if cfg!(target_feature = "avx512vnni") {
        super::avx512_vnni::adler32_avx512(adler, src)
    } else {
        unsafe { adler32_avx512_help(adler, src) }
    }
}

#[target_feature(enable = "avx512f")]
#[target_feature(enable = "avx512bw")]
unsafe fn adler32_avx512_help(adler: u32, src: &[u8]) -> u32 {
    if src.is_empty() {
        return adler;
    }

    let (before, middle, after) = unsafe { src.align_to::<__m512i>() };

    let adler = if !before.is_empty() {
        super::avx2::adler32_avx2(adler, before)
    } else {
        adler
    };

    let mut adler1 = (adler >> 16) & 0xffff;
    let mut adler0 = adler & 0xffff;

    for chunk in middle.chunks(NMAX as usize / 64) {
        (adler0, adler1) = unsafe { helper_64_bytes(adler0, adler1, chunk) };
    }

    if after.is_empty() {
        adler0 | (adler1 << 16)
    } else {
        super::avx2::adler32_avx2(adler0 | (adler1 << 16), after)
    }
}

unsafe fn helper_64_bytes(mut adler0: u32, mut adler1: u32, src: &[__m512i]) -> (u32, u32) {
    unsafe {
        let mut vs1 = _mm512_zextsi128_si512(_mm_cvtsi32_si128(adler0 as i32));
        let mut vs2 = _mm512_zextsi128_si512(_mm_cvtsi32_si128(adler1 as i32));

        let mut vs1_0 = vs1;
        let mut vs3 = ZERO;

        let dot3v = _mm512_set1_epi16(1);

        for vbuf in src.iter().copied() {
            let vs1_sad = _mm512_sad_epu8(vbuf, ZERO);
            let v_short_sum2 = _mm512_maddubs_epi16(vbuf, DOT2V);
            vs1 = _mm512_add_epi32(vs1_sad, vs1);
            vs3 = _mm512_add_epi32(vs3, vs1_0);
            let vsum2 = _mm512_madd_epi16(v_short_sum2, dot3v);
            vs2 = _mm512_add_epi32(vsum2, vs2);
            vs1_0 = vs1;
        }

        vs3 = _mm512_slli_epi32(vs3, 6);
        vs2 = _mm512_add_epi32(vs2, vs3);

        adler0 = partial_hsum(vs1) % BASE;
        adler1 = _mm512_reduce_add_epu32(vs2) % BASE;

        (adler0, adler1)
    }
}

#[inline(always)]
pub(super) unsafe fn _mm512_reduce_add_epu32(x: __m512i) -> u32 {
    unsafe {
        let a = _mm512_extracti64x4_epi64(x, 1);
        let b = _mm512_extracti64x4_epi64(x, 0);

        let a_plus_b = _mm256_add_epi32(a, b);
        let c = _mm256_extracti128_si256(a_plus_b, 1);
        let d = _mm256_extracti128_si256(a_plus_b, 0);
        let c_plus_d = _mm_add_epi32(c, d);

        let sum1 = _mm_unpackhi_epi64(c_plus_d, c_plus_d);
        let sum2 = _mm_add_epi32(sum1, c_plus_d);
        let sum3 = _mm_shuffle_epi32(sum2, 0x01);
        let sum4 = _mm_add_epi32(sum2, sum3);

        _mm_cvtsi128_si32(sum4) as u32
    }
}

#[inline(always)]
pub(super) unsafe fn partial_hsum(x: __m512i) -> u32 {
    unsafe {
        let perm_vec: __m512i =
            _mm512_setr_epi32(0, 2, 4, 6, 8, 10, 12, 14, 1, 1, 1, 1, 1, 1, 1, 1);

        let non_zero = _mm512_permutexvar_epi32(perm_vec, x);

        let non_zero_avx = _mm512_castsi512_si256(non_zero);

        let sum1 = _mm_add_epi32(
            _mm256_extracti128_si256(non_zero_avx, 1),
            _mm256_castsi256_si128(non_zero_avx),
        );
        let sum2 = _mm_add_epi32(sum1, _mm_unpackhi_epi64(sum1, sum1));
        let sum3 = _mm_add_epi32(sum2, _mm_shuffle_epi32(sum2, 1));

        _mm_cvtsi128_si32(sum3) as u32
    }
}
