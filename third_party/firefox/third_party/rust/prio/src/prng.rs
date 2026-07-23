// Copyright (c) 2020 Apple Inc.
// SPDX-License-Identifier: MPL-2.0

//! Tool for generating pseudorandom field elements.
//!
//! NOTE: The public API for this module is a work in progress.

use crate::field::{FieldElement, FieldElementExt};
#[cfg(all(feature = "crypto-dependencies", feature = "experimental"))]
use crate::vdaf::xof::SeedStreamAes128;
use crate::vdaf::xof::{Seed, SeedStreamTurboShake128, Xof, XofTurboShake128};
use rand_core::RngCore;

use std::marker::PhantomData;
use std::ops::ControlFlow;

const BUFFER_SIZE_IN_ELEMENTS: usize = 32;

/// Errors propagated by methods in this module.
#[derive(Debug, thiserror::Error)]
#[non_exhaustive]
pub enum PrngError {
    /// Failure when calling getrandom().
    #[error("getrandom: {0}")]
    GetRandom(#[from] getrandom::Error),
}

/// This type implements an iterator that generates a pseudorandom sequence of field elements. The
/// sequence is derived from a XOF's key stream.
#[derive(Debug)]
pub(crate) struct Prng<F, S> {
    phantom: PhantomData<F>,
    seed_stream: S,
    buffer: Vec<u8>,
    buffer_index: usize,
}

#[cfg(all(feature = "crypto-dependencies", feature = "experimental"))]
impl<F: FieldElement> Prng<F, SeedStreamAes128> {
    /// Create a [`Prng`] from a seed for Prio 2. The first 16 bytes of the seed and the last 16
    /// bytes of the seed are used, respectively, for the key and initialization vector for AES128
    /// in CTR mode.
    pub(crate) fn from_prio2_seed(seed: &[u8; 32]) -> Self {
        let seed_stream = SeedStreamAes128::new(&seed[..16], &seed[16..]);
        Self::from_seed_stream(seed_stream)
    }
}

impl<F: FieldElement> Prng<F, SeedStreamTurboShake128> {
    /// Create a [`Prng`] from a randomly generated seed.
    pub(crate) fn new() -> Result<Self, PrngError> {
        let seed = Seed::generate()?;
        Ok(Prng::from_seed_stream(XofTurboShake128::seed_stream(
            &seed,
            &[],
            &[],
        )))
    }
}

impl<F, S> Prng<F, S>
where
    F: FieldElement,
    S: RngCore,
{
    pub(crate) fn from_seed_stream(mut seed_stream: S) -> Self {
        let mut buffer = vec![0; BUFFER_SIZE_IN_ELEMENTS * F::ENCODED_SIZE];
        seed_stream.fill_bytes(&mut buffer);

        Self {
            phantom: PhantomData::<F>,
            seed_stream,
            buffer,
            buffer_index: 0,
        }
    }

    pub(crate) fn get(&mut self) -> F {
        loop {
            for i in (self.buffer_index..self.buffer.len()).step_by(F::ENCODED_SIZE) {
                let j = i + F::ENCODED_SIZE;

                if j > self.buffer.len() {
                    break;
                }

                self.buffer_index = j;

                match F::from_random_rejection(&self.buffer[i..j]) {
                    ControlFlow::Break(x) => return x,
                    ControlFlow::Continue(()) => continue, 
                }
            }

            let left_over = self.buffer.len() - self.buffer_index;
            self.buffer.copy_within(self.buffer_index.., 0);
            self.seed_stream.fill_bytes(&mut self.buffer[left_over..]);
            self.buffer_index = 0;
        }
    }

    /// Convert this object into a field element generator for a different field.
    #[cfg(all(feature = "crypto-dependencies", feature = "experimental"))]
    pub(crate) fn into_new_field<F1: FieldElement>(self) -> Prng<F1, S> {
        Prng {
            phantom: PhantomData,
            seed_stream: self.seed_stream,
            buffer: self.buffer,
            buffer_index: self.buffer_index,
        }
    }
}

impl<F, S> Iterator for Prng<F, S>
where
    F: FieldElement,
    S: RngCore,
{
    type Item = F;

    fn next(&mut self) -> Option<F> {
        Some(self.get())
    }
}
