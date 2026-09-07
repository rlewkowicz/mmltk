pub fn rintf(x: f32) -> f32 {
    let one_over_e = 1.0 / f32::EPSILON;
    let as_u32: u32 = x.to_bits();
    let exponent: u32 = as_u32 >> 23 & 0xff;
    let is_positive = (as_u32 >> 31) == 0;
    if exponent >= 0x7f + 23 {
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

