/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunSoft, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 *
 */

use super::atan;
use super::fabs;

const PI: f64 = 3.1415926535897931160E+00; 
const PI_LO: f64 = 1.2246467991473531772E-16; 

/// Arctangent of y/x (f64)
///
/// Computes the inverse tangent (arc tangent) of `y/x`.
/// Produces the correct result even for angles near pi/2 or -pi/2 (that is, when `x` is near 0).
/// Returns a value in radians, in the range of -pi to pi.
pub fn atan2(y: f64, x: f64) -> f64 {
    if x.is_nan() || y.is_nan() {
        return x + y;
    }
    let mut ix = (x.to_bits() >> 32) as u32;
    let lx = x.to_bits() as u32;
    let mut iy = (y.to_bits() >> 32) as u32;
    let ly = y.to_bits() as u32;
    if ((ix.wrapping_sub(0x3ff00000)) | lx) == 0 {
        return atan(y);
    }
    let m = ((iy >> 31) & 1) | ((ix >> 30) & 2); 
    ix &= 0x7fffffff;
    iy &= 0x7fffffff;

    if (iy | ly) == 0 {
        return match m {
            0 | 1 => y, 
            2 => PI,    
            _ => -PI,   
        };
    }
    if (ix | lx) == 0 {
        return if m & 1 != 0 { -PI / 2.0 } else { PI / 2.0 };
    }
    if ix == 0x7ff00000 {
        if iy == 0x7ff00000 {
            return match m {
                0 => PI / 4.0,        
                1 => -PI / 4.0,       
                2 => 3.0 * PI / 4.0,  
                _ => -3.0 * PI / 4.0, 
            };
        } else {
            return match m {
                0 => 0.0,  
                1 => -0.0, 
                2 => PI,   
                _ => -PI,  
            };
        }
    }
    if ix.wrapping_add(64 << 20) < iy || iy == 0x7ff00000 {
        return if m & 1 != 0 { -PI / 2.0 } else { PI / 2.0 };
    }

    let z = if (m & 2 != 0) && iy.wrapping_add(64 << 20) < ix {
        0.0
    } else {
        atan(fabs(y / x))
    };
    match m {
        0 => z,                
        1 => -z,               
        2 => PI - (z - PI_LO), 
        _ => (z - PI_LO) - PI, 
    }
}
