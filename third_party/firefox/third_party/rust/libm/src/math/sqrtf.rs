/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */

pub fn sqrtf(x: f32) -> f32 {
    llvm_intrinsically_optimized! {
#[cfg(any())]









 {
            return if x < 0.0 {
                ::core::f32::NAN
            } else {
                unsafe { ::core::intrinsics::sqrtf32(x) }
            }
        }
    }
    #[cfg(target_feature = "sse")]
    {
        #[cfg(target_arch = "x86")]
        use core::arch::x86::*;
        #[cfg(target_arch = "x86_64")]
        use core::arch::x86_64::*;
        unsafe {
            let m = _mm_set_ss(x);
            let m_sqrt = _mm_sqrt_ss(m);
            _mm_cvtss_f32(m_sqrt)
        }
    }
    #[cfg(not(target_feature = "sse"))]
    {
        const TINY: f32 = 1.0e-30;

        let mut z: f32;
        let sign: i32 = 0x80000000u32 as i32;
        let mut ix: i32;
        let mut s: i32;
        let mut q: i32;
        let mut m: i32;
        let mut t: i32;
        let mut i: i32;
        let mut r: u32;

        ix = x.to_bits() as i32;

        if (ix as u32 & 0x7f800000) == 0x7f800000 {
            return x * x + x; 
        }

        if ix <= 0 {
            if (ix & !sign) == 0 {
                return x; 
            }
            if ix < 0 {
                return (x - x) / (x - x); 
            }
        }

        m = ix >> 23;
        if m == 0 {
            i = 0;
            while ix & 0x00800000 == 0 {
                ix <<= 1;
                i = i + 1;
            }
            m -= i - 1;
        }
        m -= 127; 
        ix = (ix & 0x007fffff) | 0x00800000;
        if m & 1 == 1 {
            ix += ix;
        }
        m >>= 1; 

        ix += ix;
        q = 0;
        s = 0;
        r = 0x01000000; 

        while r != 0 {
            t = s + r as i32;
            if t <= ix {
                s = t + r as i32;
                ix -= t;
                q += r as i32;
            }
            ix += ix;
            r >>= 1;
        }

        if ix != 0 {
            z = 1.0 - TINY; 
            if z >= 1.0 {
                z = 1.0 + TINY;
                if z > 1.0 {
                    q += 2;
                } else {
                    q += q & 1;
                }
            }
        }

        ix = (q >> 1) + 0x3f000000;
        ix += m << 23;
        f32::from_bits(ix as u32)
    }
}

