use core::arch::x86_64::{
    __m512i, _mm512_add_epi32, _mm512_dpbusd_epi32, _mm512_loadu_si512, _mm512_sad_epu8,
    _mm512_setzero_si512, _mm512_slli_epi32, _mm512_zextsi128_si512, _mm_cvtsi32_si128,
};

use super::avx512::{_mm512_reduce_add_epu32, partial_hsum};
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

pub fn adler32_avx512(adler: u32, src: &[u8]) -> u32 {
    assert!(cfg!(target_feature = "avx512bw"));
    assert!(cfg!(target_feature = "avx512vnni"));
    unsafe { adler32_avx512_vnni(adler, src) }
}

#[target_feature(enable = "avx512bw")]
#[target_feature(enable = "avx512vnni")]
pub(super) fn adler32_avx512_vnni(mut adler: u32, mut src: &[u8]) -> u32 {
    if src.is_empty() {
        return adler;
    }

    let mut adler0;
    let mut adler1;
    adler1 = (adler >> 16) & 0xffff;
    adler0 = adler & 0xffff;

    unsafe {
        'rem_peel: loop {
            if src.len() < 32 {
                return super::avx2::adler32_avx2(adler, src);
            }

            if src.len() < 64 {
                return super::avx2::adler32_avx2(adler, src);
            }

            let dot2v = DOT2V;

            let zero = _mm512_setzero_si512();
            let mut vs1;
            let mut vs2;

            while src.len() >= 64 {
                vs1 = _mm512_zextsi128_si512(_mm_cvtsi32_si128(adler0 as i32));
                vs2 = _mm512_zextsi128_si512(_mm_cvtsi32_si128(adler1 as i32));
                let mut k: usize = Ord::min(src.len(), NMAX as usize);
                k -= k % 64;
                let mut vs1_0 = vs1;
                let mut vs3 = _mm512_setzero_si512();
                let mut vs2_1 = _mm512_setzero_si512();
                let mut vbuf0;
                let mut vbuf1;

                if (k % 128) != 0 {
                    vbuf1 = _mm512_loadu_si512(src.as_ptr().cast::<__m512i>());

                    src = &src[64..];
                    k -= 64;

                    let vs1_sad = _mm512_sad_epu8(vbuf1, zero);
                    vs1 = _mm512_add_epi32(vs1, vs1_sad);
                    vs3 = _mm512_add_epi32(vs3, vs1_0);
                    vs2 = _mm512_dpbusd_epi32(vs2, vbuf1, dot2v);
                    vs1_0 = vs1;
                }

                while k >= 128 {
                    vbuf0 = _mm512_loadu_si512(src.as_ptr().cast::<__m512i>());
                    vbuf1 = _mm512_loadu_si512(src.as_ptr().cast::<__m512i>().add(1));
                    src = &src[128..];
                    k -= 128;

                    let mut vs1_sad = _mm512_sad_epu8(vbuf0, zero);
                    vs1 = _mm512_add_epi32(vs1, vs1_sad);
                    vs3 = _mm512_add_epi32(vs3, vs1_0);
                    vs2 = _mm512_dpbusd_epi32(vs2, vbuf0, dot2v);

                    vs3 = _mm512_add_epi32(vs3, vs1);
                    vs1_sad = _mm512_sad_epu8(vbuf1, zero);
                    vs1 = _mm512_add_epi32(vs1, vs1_sad);
                    vs2_1 = _mm512_dpbusd_epi32(vs2_1, vbuf1, dot2v);
                    vs1_0 = vs1;
                }

                vs3 = _mm512_slli_epi32(vs3, 6);
                vs2 = _mm512_add_epi32(vs2, vs3);
                vs2 = _mm512_add_epi32(vs2, vs2_1);

                adler0 = partial_hsum(vs1) % BASE;
                adler1 = _mm512_reduce_add_epu32(vs2) % BASE;
            }

            adler = adler0 | (adler1 << 16);

            if !src.is_empty() {
                continue 'rem_peel;
            }

            return adler;
        }
    }
}
