// Copyright (c) 2020 Apple Inc.
// SPDX-License-Identifier: MPL-2.0

//! Primitives for the Prio2 server.
use crate::{
    field::{FftFriendlyFieldElement, FieldError},
    polynomial::poly_interpret_eval,
    prng::PrngError,
    vdaf::prio2::client::{unpack_proof, SerializeError},
};
use serde::{Deserialize, Serialize};

/// Possible errors from server operations
#[derive(Debug, thiserror::Error)]
pub enum ServerError {
    /// Unexpected Share Length
    #[allow(unused)]
    #[error("unexpected share length")]
    ShareLength,
    /// Finite field operation error
    #[error("finite field operation error")]
    Field(#[from] FieldError),
    /// Serialization/deserialization error
    #[error("serialization/deserialization error")]
    Serialize(#[from] SerializeError),
    /// Failure when calling getrandom().
    #[error("getrandom: {0}")]
    GetRandom(#[from] getrandom::Error),
    /// PRNG error.
    #[error("prng error: {0}")]
    Prng(#[from] PrngError),
}

/// Verification message for proof validation
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct VerificationMessage<F> {
    /// f evaluated at random point
    pub f_r: F,
    /// g evaluated at random point
    pub g_r: F,
    /// h evaluated at random point
    pub h_r: F,
}

/// Given a proof and evaluation point, this constructs the verification
/// message.
pub(crate) fn generate_verification_message<F: FftFriendlyFieldElement>(
    dimension: usize,
    eval_at: F,
    proof: &[F],
    is_first_server: bool,
) -> Result<VerificationMessage<F>, ServerError> {
    let unpacked = unpack_proof(proof, dimension)?;
    let n: usize = (dimension + 1).next_power_of_two();
    let proof_length = 2 * n;
    let mut fft_in = vec![F::zero(); proof_length];
    let mut fft_mem = vec![F::zero(); proof_length];

    fft_in[0] = *unpacked.f0;
    fft_in[1..unpacked.data.len() + 1].copy_from_slice(unpacked.data);
    let f_r = poly_interpret_eval(&fft_in[..n], eval_at, &mut fft_mem);

    fft_in[0] = *unpacked.g0;
    if is_first_server {
        for x in fft_in[1..unpacked.data.len() + 1].iter_mut() {
            *x -= F::one();
        }
    }
    let g_r = poly_interpret_eval(&fft_in[..n], eval_at, &mut fft_mem);

    fft_in[0] = *unpacked.h0;
    fft_in[1] = unpacked.points_h_packed[0];
    for (x, chunk) in unpacked.points_h_packed[1..]
        .iter()
        .zip(fft_in[2..proof_length].chunks_exact_mut(2))
    {
        chunk[0] = F::zero();
        chunk[1] = *x;
    }
    let h_r = poly_interpret_eval(&fft_in, eval_at, &mut fft_mem);

    Ok(VerificationMessage { f_r, g_r, h_r })
}

/// Decides if the distributed proof is valid
pub(crate) fn is_valid_share<F: FftFriendlyFieldElement>(
    v1: &VerificationMessage<F>,
    v2: &VerificationMessage<F>,
) -> bool {
    let f_r = v1.f_r + v2.f_r;
    let g_r = v1.g_r + v2.g_r;
    let h_r = v1.h_r + v2.h_r;
    f_r * g_r == h_r
}
