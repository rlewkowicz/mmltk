// Copyright 2018 Developers of the Rand project.
// Copyright 2016-2017 The Rust Project Developers.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! The binomial distribution.

use crate::{Distribution, Uniform};
use rand::Rng;
use core::fmt;
use core::cmp::Ordering;
#[allow(unused_imports)]
use num_traits::Float;

/// The binomial distribution `Binomial(n, p)`.
///
/// This distribution has density function:
/// `f(k) = n!/(k! (n-k)!) p^k (1-p)^(n-k)` for `k >= 0`.
///
/// # Example
///
/// ```
/// use rand_distr::{Binomial, Distribution};
///
/// let bin = Binomial::new(20, 0.3).unwrap();
/// let v = bin.sample(&mut rand::thread_rng());
/// println!("{} is from a binomial distribution", v);
/// ```
#[derive(Clone, Copy, Debug)]
#[cfg_attr(feature = "serde1", derive(serde::Serialize, serde::Deserialize))]
pub struct Binomial {
    /// Number of trials.
    n: u64,
    /// Probability of success.
    p: f64,
}

/// Error type returned from `Binomial::new`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// `p < 0` or `nan`.
    ProbabilityTooSmall,
    /// `p > 1`.
    ProbabilityTooLarge,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Error::ProbabilityTooSmall => "p < 0 or is NaN in binomial distribution",
            Error::ProbabilityTooLarge => "p > 1 in binomial distribution",
        })
    }
}

#[cfg(feature = "std")]
#[cfg_attr(doc_cfg, doc(cfg(feature = "std")))]
impl std::error::Error for Error {}

impl Binomial {
    /// Construct a new `Binomial` with the given shape parameters `n` (number
    /// of trials) and `p` (probability of success).
    pub fn new(n: u64, p: f64) -> Result<Binomial, Error> {
        if !(p >= 0.0) {
            return Err(Error::ProbabilityTooSmall);
        }
        if !(p <= 1.0) {
            return Err(Error::ProbabilityTooLarge);
        }
        Ok(Binomial { n, p })
    }
}

/// Convert a `f64` to an `i64`, panicking on overflow.
fn f64_to_i64(x: f64) -> i64 {
    assert!(x < (core::i64::MAX as f64));
    x as i64
}

impl Distribution<u64> for Binomial {
    #[allow(clippy::many_single_char_names)] 
    fn sample<R: Rng + ?Sized>(&self, rng: &mut R) -> u64 {
        if self.p == 0.0 {
            return 0;
        } else if self.p == 1.0 {
            return self.n;
        }

        let p = if self.p <= 0.5 { self.p } else { 1.0 - self.p };

        let result;
        let q = 1. - p;


        const BINV_THRESHOLD: f64 = 10.;

        if (self.n as f64) * p < BINV_THRESHOLD && self.n <= (core::i32::MAX as u64) {
            let s = p / q;
            let a = ((self.n + 1) as f64) * s;
            let mut r = q.powi(self.n as i32);
            let mut u: f64 = rng.gen();
            let mut x = 0;
            while u > r as f64 {
                u -= r;
                x += 1;
                r *= a / (x as f64) - s;
            }
            result = x;
        } else {

            const SQUEEZE_THRESHOLD: i64 = 20;

            let n = self.n as f64;
            let np = n * p;
            let npq = np * q;
            let f_m = np + p;
            let m = f64_to_i64(f_m);
            let p1 = (2.195 * npq.sqrt() - 4.6 * q).floor() + 0.5;
            let x_m = (m as f64) + 0.5;
            let x_l = x_m - p1;
            let x_r = x_m + p1;
            let c = 0.134 + 20.5 / (15.3 + (m as f64));
            let p2 = p1 * (1. + 2. * c);

            fn lambda(a: f64) -> f64 {
                a * (1. + 0.5 * a)
            }

            let lambda_l = lambda((f_m - x_l) / (f_m - x_l * p));
            let lambda_r = lambda((x_r - f_m) / (x_r * q));
            let p3 = p2 + c / lambda_l;
            let p4 = p3 + c / lambda_r;

            let mut y: i64;

            let gen_u = Uniform::new(0., p4);
            let gen_v = Uniform::new(0., 1.);

            loop {
                let u = gen_u.sample(rng);
                let mut v = gen_v.sample(rng);
                if !(u > p1) {
                    y = f64_to_i64(x_m - p1 * v + u);
                    break;
                }

                if !(u > p2) {
                    let x = x_l + (u - p1) / c;
                    v = v * c + 1.0 - (x - x_m).abs() / p1;
                    if v > 1. {
                        continue;
                    } else {
                        y = f64_to_i64(x);
                    }
                } else if !(u > p3) {
                    y = f64_to_i64(x_l + v.ln() / lambda_l);
                    if y < 0 {
                        continue;
                    } else {
                        v *= (u - p2) * lambda_l;
                    }
                } else {
                    y = f64_to_i64(x_r - v.ln() / lambda_r);
                    if y > 0 && (y as u64) > self.n {
                        continue;
                    } else {
                        v *= (u - p3) * lambda_r;
                    }
                }


                let k = (y - m).abs();
                if !(k > SQUEEZE_THRESHOLD && (k as f64) < 0.5 * npq - 1.) {
                    let s = p / q;
                    let a = s * (n + 1.);
                    let mut f = 1.0;
                    match m.cmp(&y) {
                        Ordering::Less => {
                            let mut i = m;
                            loop {
                                i += 1;
                                f *= a / (i as f64) - s;
                                if i == y {
                                    break;
                                }
                            }
                        },
                        Ordering::Greater => {
                            let mut i = y;
                            loop {
                                i += 1;
                                f /= a / (i as f64) - s;
                                if i == m {
                                    break;
                                }
                            }
                        },
                        Ordering::Equal => {},
                    }
                    if v > f {
                        continue;
                    } else {
                        break;
                    }
                }

                let k = k as f64;
                let rho = (k / npq) * ((k * (k / 3. + 0.625) + 1. / 6.) / npq + 0.5);
                let t = -0.5 * k * k / npq;
                let alpha = v.ln();
                if alpha < t - rho {
                    break;
                }
                if alpha > t + rho {
                    continue;
                }

                let x1 = (y + 1) as f64;
                let f1 = (m + 1) as f64;
                let z = (f64_to_i64(n) + 1 - m) as f64;
                let w = (f64_to_i64(n) - y + 1) as f64;

                fn stirling(a: f64) -> f64 {
                    let a2 = a * a;
                    (13860. - (462. - (132. - (99. - 140. / a2) / a2) / a2) / a2) / a / 166320.
                }

                if alpha
                    > x_m * (f1 / x1).ln()
                        + (n - (m as f64) + 0.5) * (z / w).ln()
                        + ((y - m) as f64) * (w * p / (x1 * q)).ln()
                        + stirling(f1)
                        + stirling(z)
                        - stirling(x1)
                        - stirling(w)
                {
                    continue;
                }

                break;
            }
            assert!(y >= 0);
            result = y as u64;
        }

        if p != self.p {
            self.n - result
        } else {
            result
        }
    }
}
