use super::{log, log1p, sqrt};

const LN2: f64 = 0.693147180559945309417232121458176568; 

/// Inverse hyperbolic cosine (f64)
///
/// Calculates the inverse hyperbolic cosine of `x`.
/// Is defined as `log(x + sqrt(x*x-1))`.
/// `x` must be a number greater than or equal to 1.
pub fn acosh(x: f64) -> f64 {
    let u = x.to_bits();
    let e = ((u >> 52) as usize) & 0x7ff;


    if e < 0x3ff + 1 {
        return log1p(x - 1.0 + sqrt((x - 1.0) * (x - 1.0) + 2.0 * (x - 1.0)));
    }
    if e < 0x3ff + 26 {
        return log(2.0 * x - 1.0 / (x + sqrt(x * x - 1.0)));
    }
    return log(x) + LN2;
}
