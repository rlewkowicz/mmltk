pub fn rint(x: f64) -> f64 {
    let one_over_e = 1.0 / f64::EPSILON;
    let as_u64: u64 = x.to_bits();
    let exponent: u64 = as_u64 >> 52 & 0x7ff;
    let is_positive = (as_u64 >> 63) == 0;
    if exponent >= 0x3ff + 52 {
        x
    } else {
        let ans = if is_positive {
            x + one_over_e - one_over_e
        } else {
            x - one_over_e + one_over_e
        };

        if ans == 0.0 {
            if is_positive {
                0.0
            } else {
                -0.0
            }
        } else {
            ans
        }
    }
}

