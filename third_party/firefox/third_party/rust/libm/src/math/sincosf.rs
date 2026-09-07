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

use super::{k_cosf, k_sinf, rem_pio2f};

const PI_2: f32 = 0.5 * 3.1415926535897931160E+00;
const S1PIO2: f32 = 1.0 * PI_2; 
const S2PIO2: f32 = 2.0 * PI_2; 
const S3PIO2: f32 = 3.0 * PI_2; 
const S4PIO2: f32 = 4.0 * PI_2; 

pub fn sincosf(x: f32) -> (f32, f32) {
    let s: f32;
    let c: f32;
    let mut ix: u32;
    let sign: bool;

    ix = x.to_bits();
    sign = (ix >> 31) != 0;
    ix &= 0x7fffffff;

    if ix <= 0x3f490fda {
        if ix < 0x39800000 {

            let x1p120 = f32::from_bits(0x7b800000); 
            if ix < 0x00100000 {
                force_eval!(x / x1p120);
            } else {
                force_eval!(x + x1p120);
            }
            return (x, 1.0);
        }
        return (k_sinf(x as f64), k_cosf(x as f64));
    }

    if ix <= 0x407b53d1 {
        if ix <= 0x4016cbe3 {
            if sign {
                s = -k_cosf((x + S1PIO2) as f64);
                c = k_sinf((x + S1PIO2) as f64);
            } else {
                s = k_cosf((S1PIO2 - x) as f64);
                c = k_sinf((S1PIO2 - x) as f64);
            }
        }
        else {
            if sign {
                s = -k_sinf((x + S2PIO2) as f64);
                c = -k_cosf((x + S2PIO2) as f64);
            } else {
                s = -k_sinf((x - S2PIO2) as f64);
                c = -k_cosf((x - S2PIO2) as f64);
            }
        }

        return (s, c);
    }

    if ix <= 0x40e231d5 {
        if ix <= 0x40afeddf {
            if sign {
                s = k_cosf((x + S3PIO2) as f64);
                c = -k_sinf((x + S3PIO2) as f64);
            } else {
                s = -k_cosf((x - S3PIO2) as f64);
                c = k_sinf((x - S3PIO2) as f64);
            }
        } else {
            if sign {
                s = k_sinf((x + S4PIO2) as f64);
                c = k_cosf((x + S4PIO2) as f64);
            } else {
                s = k_sinf((x - S4PIO2) as f64);
                c = k_cosf((x - S4PIO2) as f64);
            }
        }

        return (s, c);
    }

    if ix >= 0x7f800000 {
        let rv = x - x;
        return (rv, rv);
    }

    let (n, y) = rem_pio2f(x);
    s = k_sinf(y);
    c = k_cosf(y);
    match n & 3 {
        0 => (s, c),
        1 => (c, -s),
        2 => (-s, -c),
        3 => (-c, s),
        #[cfg(debug_assertions)]
        _ => unreachable!(),
        #[cfg(not(debug_assertions))]
        _ => (0.0, 1.0),
    }
}

