// SPDX-License-Identifier: MPL-2.0

//! This module implements an iterative FFT algorithm for computing the (inverse) Discrete Fourier
//! Transform (DFT) over a slice of field elements.

use crate::field::FftFriendlyFieldElement;
use crate::fp::{log2, MAX_ROOTS};

use std::convert::TryFrom;

/// An error returned by an FFT operation.
#[derive(Debug, PartialEq, Eq, thiserror::Error)]
#[non_exhaustive]
pub enum FftError {
    /// The output is too small.
    #[error("output slice is smaller than specified size")]
    OutputTooSmall,
    /// The specified size is too large.
    #[error("size is larger than than maximum permitted")]
    SizeTooLarge,
    /// The specified size is not a power of 2.
    #[error("size is not a power of 2")]
    SizeInvalid,
}

/// Sets `outp` to the DFT of `inp`.
///
/// Interpreting the input as the coefficients of a polynomial, the output is equal to the input
/// evaluated at points `p^0, p^1, ... p^(size-1)`, where `p` is the `2^size`-th principal root of
/// unity.
#[allow(clippy::many_single_char_names)]
pub fn discrete_fourier_transform<F: FftFriendlyFieldElement>(
    outp: &mut [F],
    inp: &[F],
    size: usize,
) -> Result<(), FftError> {
    let d = usize::try_from(log2(size as u128)).map_err(|_| FftError::SizeTooLarge)?;

    if size > outp.len() {
        return Err(FftError::OutputTooSmall);
    }

    if size > 1 << MAX_ROOTS {
        return Err(FftError::SizeTooLarge);
    }

    if size != 1 << d {
        return Err(FftError::SizeInvalid);
    }

    for (i, outp_val) in outp[..size].iter_mut().enumerate() {
        let j = bitrev(d, i);
        *outp_val = if j < inp.len() { inp[j] } else { F::zero() };
    }

    let mut w: F;
    for l in 1..d + 1 {
        w = F::one();
        let r = F::root(l).unwrap();
        let y = 1 << (l - 1);
        let chunk = (size / y) >> 1;

        for j in 0..chunk {
            let x = j << l;
            let u = outp[x];
            let v = outp[x + y];
            outp[x] = u + v;
            outp[x + y] = u - v;
        }

        for i in 1..y {
            w *= r;
            for j in 0..chunk {
                let x = (j << l) + i;
                let u = outp[x];
                let v = w * outp[x + y];
                outp[x] = u + v;
                outp[x + y] = u - v;
            }
        }
    }

    Ok(())
}

/// Sets `outp` to the inverse of the DFT of `inp`.

/// An intermediate step in the computation of the inverse DFT. Exposing this function allows us to
/// amortize the cost the modular inverse across multiple inverse DFT operations.
pub(crate) fn discrete_fourier_transform_inv_finish<F: FftFriendlyFieldElement>(
    outp: &mut [F],
    size: usize,
    size_inv: F,
) {
    let mut tmp: F;
    outp[0] *= size_inv;
    outp[size >> 1] *= size_inv;
    for i in 1..size >> 1 {
        tmp = outp[i] * size_inv;
        outp[i] = outp[size - i] * size_inv;
        outp[size - i] = tmp;
    }
}

fn bitrev(d: usize, x: usize) -> usize {
    let mut y = 0;
    for i in 0..d {
        y += ((x >> i) & 1) << (d - i);
    }
    y >> 1
}
