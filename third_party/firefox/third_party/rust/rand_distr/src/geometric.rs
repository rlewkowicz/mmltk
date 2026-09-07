//! The geometric distribution.

use crate::Distribution;
use rand::Rng;
use core::fmt;
#[allow(unused_imports)]
use num_traits::Float;

/// The geometric distribution `Geometric(p)` bounded to `[0, u64::MAX]`.
/// 
/// This is the probability distribution of the number of failures before the
/// first success in a series of Bernoulli trials. It has the density function
/// `f(k) = (1 - p)^k p` for `k >= 0`, where `p` is the probability of success
/// on each trial.
/// 
/// This is the discrete analogue of the [exponential distribution](crate::Exp).
/// 
/// Note that [`StandardGeometric`](crate::StandardGeometric) is an optimised
/// implementation for `p = 0.5`.
///
/// # Example
///
/// ```
/// use rand_distr::{Geometric, Distribution};
///
/// let geo = Geometric::new(0.25).unwrap();
/// let v = geo.sample(&mut rand::thread_rng());
/// println!("{} is from a Geometric(0.25) distribution", v);
/// ```
#[derive(Copy, Clone, Debug)]
#[cfg_attr(feature = "serde1", derive(serde::Serialize, serde::Deserialize))]
pub struct Geometric
{
    p: f64,
    pi: f64,
    k: u64
}

/// Error type returned from `Geometric::new`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// `p < 0 || p > 1` or `nan`
    InvalidProbability,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Error::InvalidProbability => "p is NaN or outside the interval [0, 1] in geometric distribution",
        })
    }
}

#[cfg(feature = "std")]
#[cfg_attr(doc_cfg, doc(cfg(feature = "std")))]
impl std::error::Error for Error {}

impl Geometric {
    /// Construct a new `Geometric` with the given shape parameter `p`
    /// (probability of success on each trial).
    pub fn new(p: f64) -> Result<Self, Error> {
        if !p.is_finite() || p < 0.0 || p > 1.0 {
            Err(Error::InvalidProbability)
        } else if p == 0.0 || p >= 2.0 / 3.0 {
            Ok(Geometric { p, pi: p, k: 0 })
        } else {
            let (pi, k) = {
                let mut k = 1;
                let mut pi = (1.0 - p).powi(2);
                while pi > 0.5 {
                    k += 1;
                    pi = pi * pi;
                }
                (pi, k)
            };

            Ok(Geometric { p, pi, k })
        }
    }
}

impl Distribution<u64> for Geometric
{
    fn sample<R: Rng + ?Sized>(&self, rng: &mut R) -> u64 {
        if self.p >= 2.0 / 3.0 {
            let mut failures = 0;
            loop {
                let u = rng.gen::<f64>();
                if u <= self.p { break; }
                failures += 1;
            }
            return failures;
        }
        
        if self.p == 0.0 { return core::u64::MAX; }

        let Geometric { p, pi, k } = *self;


        let d = {
            let mut failures = 0;
            while rng.gen::<f64>() < pi {
                failures += 1;
            }
            failures
        };

        let m = loop {
            let m = rng.gen::<u64>() & ((1 << k) - 1);
            let p_reject = if m <= core::i32::MAX as u64 {
                (1.0 - p).powi(m as i32)
            } else {
                (1.0 - p).powf(m as f64)
            };
            
            let u = rng.gen::<f64>();
            if u < p_reject {
                break m;
            }
        };

        (d << k) + m
    }
}

/// Samples integers according to the geometric distribution with success
/// probability `p = 0.5`. This is equivalent to `Geometeric::new(0.5)`,
/// but faster.
/// 
/// See [`Geometric`](crate::Geometric) for the general geometric distribution.
/// 
/// Implemented via iterated [Rng::gen::<u64>().leading_zeros()].
/// 
/// # Example
/// ```
/// use rand::prelude::*;
/// use rand_distr::StandardGeometric;
/// 
/// let v = StandardGeometric.sample(&mut thread_rng());
/// println!("{} is from a Geometric(0.5) distribution", v);
/// ```
#[derive(Copy, Clone, Debug)]
#[cfg_attr(feature = "serde1", derive(serde::Serialize, serde::Deserialize))]
pub struct StandardGeometric;

impl Distribution<u64> for StandardGeometric {
    fn sample<R: Rng + ?Sized>(&self, rng: &mut R) -> u64 {
        let mut result = 0;
        loop {
            let x = rng.gen::<u64>().leading_zeros() as u64;
            result += x;
            if x < 64 { break; }
        }
        result
    }
}
