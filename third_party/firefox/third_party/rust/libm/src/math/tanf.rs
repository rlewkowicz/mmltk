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

use super::{k_tanf, rem_pio2f};

use core::f64::consts::FRAC_PI_2;

const T1_PIO2: f64 = 1. * FRAC_PI_2; 
const T2_PIO2: f64 = 2. * FRAC_PI_2; 
const T3_PIO2: f64 = 3. * FRAC_PI_2; 
const T4_PIO2: f64 = 4. * FRAC_PI_2; 

pub fn tanf(x: f32) -> f32 {
    let x64 = x as f64;

    let x1p120 = f32::from_bits(0x7b800000); 

    let mut ix = x.to_bits();
    let sign = (ix >> 31) != 0;
    ix &= 0x7fffffff;

    if ix <= 0x3f490fda {
        if ix < 0x39800000 {
            force_eval!(if ix < 0x00800000 {
                x / x1p120
            } else {
                x + x1p120
            });
            return x;
        }
        return k_tanf(x64, false);
    }
    if ix <= 0x407b53d1 {
        if ix <= 0x4016cbe3 {
            return k_tanf(if sign { x64 + T1_PIO2 } else { x64 - T1_PIO2 }, true);
        } else {
            return k_tanf(if sign { x64 + T2_PIO2 } else { x64 - T2_PIO2 }, false);
        }
    }
    if ix <= 0x40e231d5 {
        if ix <= 0x40afeddf {
            return k_tanf(if sign { x64 + T3_PIO2 } else { x64 - T3_PIO2 }, true);
        } else {
            return k_tanf(if sign { x64 + T4_PIO2 } else { x64 - T4_PIO2 }, false);
        }
    }

    if ix >= 0x7f800000 {
        return x - x;
    }

    let (n, y) = rem_pio2f(x);
    k_tanf(y, n & 1 != 0)
}
