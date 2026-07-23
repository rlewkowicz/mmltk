// Copyright (c) 2023 ISRG
// SPDX-License-Identifier: MPL-2.0
// This file contains code covered by the following copyright and permission notice
// Copyright (c) 2022 President and Fellows of Harvard College
// Permission is hereby granted, free of charge, to any person obtaining a copy
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// The above copyright notice and this permission notice shall be included in all
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// This file incorporates work covered by the following copyright and
//   Copyright 2020 Thomas Steinke
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//       http://www.apache.org/licenses/LICENSE-2.0
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.


//! Implementation of a sampler from the Discrete Gaussian Distribution.
//!
//! Follows
//!     Clément Canonne, Gautam Kamath, Thomas Steinke. The Discrete Gaussian for Differential Privacy. 2020.
//!     <https://arxiv.org/pdf/2004.00010.pdf>

use num_bigint::{BigInt, BigUint, UniformBigUint};
use num_integer::Integer;
use num_iter::range_inclusive;
use num_rational::Ratio;
use num_traits::{One, Zero};
use rand::{distributions::uniform::UniformSampler, distributions::Distribution, Rng};
use serde::{Deserialize, Serialize};

use super::{
    DifferentialPrivacyBudget, DifferentialPrivacyDistribution, DifferentialPrivacyStrategy,
    DpError, ZCdpBudget,
};

/// Sample from the Bernoulli(gamma) distribution, where $gamma /leq 1$.
///
/// `sample_bernoulli(gamma, rng)` returns numbers distributed as $Bernoulli(gamma)$.
/// using the given random number generator for base randomness. The procedure is as described
/// on page 30 of [[CKS20]].
///
/// [CKS20]: https://arxiv.org/pdf/2004.00010.pdf
fn sample_bernoulli<R: Rng + ?Sized>(gamma: &Ratio<BigUint>, rng: &mut R) -> bool {
    let d = gamma.denom();
    assert!(!d.is_zero());
    assert!(gamma <= &Ratio::<BigUint>::one());

    let s = UniformBigUint::sample_single_inclusive(BigUint::one(), d, rng);

    s <= *gamma.numer()
}

/// Sample from the Bernoulli(exp(-gamma)) distribution where `gamma` is in `[0,1]`.
///
/// `sample_bernoulli_exp1(gamma, rng)` returns numbers distributed as $Bernoulli(exp(-gamma))$,
/// using the given random number generator for base randomness. Follows Algorithm 1 of [[CKS20]],
/// splitting the branches into two non-recursive functions. This is the `gamma in [0,1]` branch.
///
/// [CKS20]: https://arxiv.org/pdf/2004.00010.pdf
fn sample_bernoulli_exp1<R: Rng + ?Sized>(gamma: &Ratio<BigUint>, rng: &mut R) -> bool {
    assert!(!gamma.denom().is_zero());
    assert!(gamma <= &Ratio::<BigUint>::one());

    let mut k = BigUint::one();
    loop {
        if sample_bernoulli(&(gamma / k.clone()), rng) {
            k += 1u8;
        } else {
            return k.is_odd();
        }
    }
}

/// Sample from the Bernoulli(exp(-gamma)) distribution.
///
/// `sample_bernoulli_exp(gamma, rng)` returns numbers distributed as $Bernoulli(exp(-gamma))$,
/// using the given random number generator for base randomness. Follows Algorithm 1 of [[CKS20]],
/// splitting the branches into two non-recursive functions. This is the `gamma > 1` branch.
///
/// [CKS20]: https://arxiv.org/pdf/2004.00010.pdf
fn sample_bernoulli_exp<R: Rng + ?Sized>(gamma: &Ratio<BigUint>, rng: &mut R) -> bool {
    assert!(!gamma.denom().is_zero());
    for _ in range_inclusive(BigUint::one(), gamma.floor().to_integer()) {
        if !sample_bernoulli_exp1(&Ratio::<BigUint>::one(), rng) {
            return false;
        }
    }
    sample_bernoulli_exp1(&(gamma - gamma.floor()), rng)
}

/// Sample from the geometric distribution  with parameter 1 - exp(-gamma).
///
/// `sample_geometric_exp(gamma, rng)` returns numbers distributed according to
/// $Geometric(1 - exp(-gamma))$, using the given random number generator for base randomness.
/// The code follows all but the last three lines of Algorithm 2 in [[CKS20]].
///
/// [CKS20]: https://arxiv.org/pdf/2004.00010.pdf
fn sample_geometric_exp<R: Rng + ?Sized>(gamma: &Ratio<BigUint>, rng: &mut R) -> BigUint {
    let (s, t) = (gamma.numer(), gamma.denom());
    assert!(!t.is_zero());
    if gamma.is_zero() {
        return BigUint::zero();
    }

    let usampler = UniformBigUint::new(BigUint::zero(), t);
    let mut u = usampler.sample(rng);

    while !sample_bernoulli_exp1(&Ratio::<BigUint>::new(u.clone(), t.clone()), rng) {
        u = usampler.sample(rng);
    }

    let mut v = BigUint::zero();
    loop {
        if sample_bernoulli_exp1(&Ratio::<BigUint>::one(), rng) {
            v += 1u8;
        } else {
            break;
        }
    }

    (u + t * v) / s
}

/// Sample from the discrete Laplace distribution.
///
/// `sample_discrete_laplace(scale, rng)` returns numbers distributed according to
/// $\mathcal{L}_\mathbb{Z}(0, scale)$, using the given random number generator for base randomness.
/// This follows Algorithm 2 of [[CKS20]], using a subfunction for geometric sampling.
///
/// [CKS20]: https://arxiv.org/pdf/2004.00010.pdf
fn sample_discrete_laplace<R: Rng + ?Sized>(scale: &Ratio<BigUint>, rng: &mut R) -> BigInt {
    let (s, t) = (scale.numer(), scale.denom());
    assert!(!t.is_zero());
    if s.is_zero() {
        return BigInt::zero();
    }

    loop {
        let negative = sample_bernoulli(&Ratio::<BigUint>::new(BigUint::one(), 2u8.into()), rng);
        let y: BigInt = sample_geometric_exp(&scale.recip(), rng).into();
        if negative && y.is_zero() {
            continue;
        } else {
            return if negative { -y } else { y };
        }
    }
}

/// Sample from the discrete Gaussian distribution.
///
/// `sample_discrete_gaussian(sigma, rng)` returns `BigInt` numbers distributed as
/// $\mathcal{N}_\mathbb{Z}(0, sigma^2)$, using the given random number generator for base
/// randomness. Follows Algorithm 3 from [[CKS20]].
///
/// [CKS20]: https://arxiv.org/pdf/2004.00010.pdf
fn sample_discrete_gaussian<R: Rng + ?Sized>(sigma: &Ratio<BigUint>, rng: &mut R) -> BigInt {
    assert!(!sigma.denom().is_zero());
    if sigma.is_zero() {
        return 0.into();
    }
    let t = sigma.floor() + BigUint::one();

    let summand = sigma.pow(2) / t.clone();
    let prob = |term: Ratio<BigUint>| term.pow(2) * (sigma.pow(2) * BigUint::from(2u8)).recip();

    loop {
        let y = sample_discrete_laplace(&t, rng);

        let y_abs: Ratio<BigUint> = BigUint::new(y.to_u32_digits().1).into();

        let prob: Ratio<BigUint> = if y_abs < summand {
            prob(summand.clone() - y_abs)
        } else {
            prob(y_abs - summand.clone())
        };

        if sample_bernoulli_exp(&prob, rng) {
            return y;
        }
    }
}

/// Samples `BigInt` numbers according to the discrete Gaussian distribution with mean zero.
/// The distribution is defined over the integers, represented by arbitrary-precision integers.
/// The sampling procedure follows [[CKS20]].
///
/// [CKS20]: https://arxiv.org/pdf/2004.00010.pdf
#[derive(Clone, Debug)]
pub struct DiscreteGaussian {
    /// The standard deviation of the distribution.
    std: Ratio<BigUint>,
}

impl DiscreteGaussian {
    /// Create a new sampler from the Discrete Gaussian Distribution with the given
    /// standard deviation and mean zero. Errors if the input has denominator zero.
    pub fn new(std: Ratio<BigUint>) -> Result<DiscreteGaussian, DpError> {
        if std.denom().is_zero() {
            return Err(DpError::ZeroDenominator);
        }
        Ok(DiscreteGaussian { std })
    }
}

impl Distribution<BigInt> for DiscreteGaussian {
    fn sample<R>(&self, rng: &mut R) -> BigInt
    where
        R: Rng + ?Sized,
    {
        sample_discrete_gaussian(&self.std, rng)
    }
}

impl DifferentialPrivacyDistribution for DiscreteGaussian {}

/// A DP strategy using the discrete gaussian distribution.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Ord, PartialOrd)]
pub struct DiscreteGaussianDpStrategy<B>
where
    B: DifferentialPrivacyBudget,
{
    budget: B,
}

/// A DP strategy using the discrete gaussian distribution providing zero-concentrated DP.
pub type ZCdpDiscreteGaussian = DiscreteGaussianDpStrategy<ZCdpBudget>;

impl DifferentialPrivacyStrategy for DiscreteGaussianDpStrategy<ZCdpBudget> {
    type Budget = ZCdpBudget;
    type Distribution = DiscreteGaussian;
    type Sensitivity = Ratio<BigUint>;

    fn from_budget(budget: ZCdpBudget) -> DiscreteGaussianDpStrategy<ZCdpBudget> {
        DiscreteGaussianDpStrategy { budget }
    }

    /// Create a new sampler from the Discrete Gaussian Distribution with a standard
    /// deviation calibrated to provide `1/2 epsilon^2` zero-concentrated differential
    /// privacy when added to the result of an integer-valued function with sensitivity
    /// `sensitivity`, following Theorem 4 from [[CKS20]]
    ///
    /// [CKS20]: https://arxiv.org/pdf/2004.00010.pdf
    fn create_distribution(
        &self,
        sensitivity: Ratio<BigUint>,
    ) -> Result<DiscreteGaussian, DpError> {
        DiscreteGaussian::new(sensitivity / self.budget.epsilon.clone())
    }
}
