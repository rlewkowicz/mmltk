// SPDX-License-Identifier: MPL-2.0

//! A collection of gadgets.

use crate::fft::{discrete_fourier_transform, discrete_fourier_transform_inv_finish};
use crate::field::FftFriendlyFieldElement;
use crate::flp::{gadget_poly_len, wire_poly_len, FlpError, Gadget};
use crate::polynomial::{poly_deg, poly_eval, poly_mul};

#[cfg(feature = "multithreaded")]
use rayon::prelude::*;

use std::any::Any;
use std::convert::TryFrom;
use std::fmt::Debug;
use std::marker::PhantomData;

/// For input polynomials larger than or equal to this threshold, gadgets will use FFT for
/// polynomial multiplication. Otherwise, the gadget uses direct multiplication.
const FFT_THRESHOLD: usize = 60;

/// An arity-2 gadget that multiples its inputs.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Mul<F: FftFriendlyFieldElement> {
    /// Size of buffer for FFT operations.
    n: usize,
    /// Inverse of `n` in `F`.
    n_inv: F,
    /// The number of times this gadget will be called.
    num_calls: usize,
}

impl<F: FftFriendlyFieldElement> Mul<F> {
    /// Return a new multiplier gadget. `num_calls` is the number of times this gadget will be
    /// called by the validity circuit.
    pub fn new(num_calls: usize) -> Self {
        let n = gadget_poly_fft_mem_len(2, num_calls);
        let n_inv = F::from(F::Integer::try_from(n).unwrap()).inv();
        Self {
            n,
            n_inv,
            num_calls,
        }
    }

    pub(crate) fn call_poly_direct(
        &mut self,
        outp: &mut [F],
        inp: &[Vec<F>],
    ) -> Result<(), FlpError> {
        let v = poly_mul(&inp[0], &inp[1]);
        outp[..v.len()].clone_from_slice(&v);
        Ok(())
    }

    pub(crate) fn call_poly_fft(&mut self, outp: &mut [F], inp: &[Vec<F>]) -> Result<(), FlpError> {
        let n = self.n;
        let mut buf = vec![F::zero(); n];

        discrete_fourier_transform(&mut buf, &inp[0], n)?;
        discrete_fourier_transform(outp, &inp[1], n)?;

        for i in 0..n {
            buf[i] *= outp[i];
        }

        discrete_fourier_transform(outp, &buf, n)?;
        discrete_fourier_transform_inv_finish(outp, n, self.n_inv);
        Ok(())
    }
}

impl<F: FftFriendlyFieldElement> Gadget<F> for Mul<F> {
    fn call(&mut self, inp: &[F]) -> Result<F, FlpError> {
        gadget_call_check(self, inp.len())?;
        Ok(inp[0] * inp[1])
    }

    fn call_poly(&mut self, outp: &mut [F], inp: &[Vec<F>]) -> Result<(), FlpError> {
        gadget_call_poly_check(self, outp, inp)?;
        if inp[0].len() >= FFT_THRESHOLD {
            self.call_poly_fft(outp, inp)
        } else {
            self.call_poly_direct(outp, inp)
        }
    }

    fn arity(&self) -> usize {
        2
    }

    fn degree(&self) -> usize {
        2
    }

    fn calls(&self) -> usize {
        self.num_calls
    }

    fn as_any(&mut self) -> &mut dyn Any {
        self
    }
}

/// An arity-1 gadget that evaluates its input on some polynomial.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PolyEval<F: FftFriendlyFieldElement> {
    poly: Vec<F>,
    /// Size of buffer for FFT operations.
    n: usize,
    /// Inverse of `n` in `F`.
    n_inv: F,
    /// The number of times this gadget will be called.
    num_calls: usize,
}

impl<F: FftFriendlyFieldElement> PolyEval<F> {
    /// Returns a gadget that evaluates its input on `poly`. `num_calls` is the number of times
    /// this gadget is called by the validity circuit.
    pub fn new(poly: Vec<F>, num_calls: usize) -> Self {
        let n = gadget_poly_fft_mem_len(poly_deg(&poly), num_calls);
        let n_inv = F::from(F::Integer::try_from(n).unwrap()).inv();
        Self {
            poly,
            n,
            n_inv,
            num_calls,
        }
    }
}

impl<F: FftFriendlyFieldElement> PolyEval<F> {
    fn call_poly_direct(&mut self, outp: &mut [F], inp: &[Vec<F>]) -> Result<(), FlpError> {
        outp[0] = self.poly[0];
        let mut x = inp[0].to_vec();
        for i in 1..self.poly.len() {
            for j in 0..x.len() {
                outp[j] += self.poly[i] * x[j];
            }

            if i < self.poly.len() - 1 {
                x = poly_mul(&x, &inp[0]);
            }
        }
        Ok(())
    }

    fn call_poly_fft(&mut self, outp: &mut [F], inp: &[Vec<F>]) -> Result<(), FlpError> {
        let n = self.n;
        let inp = &inp[0];

        let mut inp_vals = vec![F::zero(); n];
        discrete_fourier_transform(&mut inp_vals, inp, n)?;

        let mut x_vals = inp_vals.clone();
        let mut x = vec![F::zero(); n];
        x[..inp.len()].clone_from_slice(inp);

        outp[0] = self.poly[0];
        for i in 1..self.poly.len() {
            for j in 0..n {
                outp[j] += self.poly[i] * x[j];
            }

            if i < self.poly.len() - 1 {
                for j in 0..n {
                    x_vals[j] *= inp_vals[j];
                }

                discrete_fourier_transform(&mut x, &x_vals, n)?;
                discrete_fourier_transform_inv_finish(&mut x, n, self.n_inv);
            }
        }
        Ok(())
    }
}

impl<F: FftFriendlyFieldElement> Gadget<F> for PolyEval<F> {
    fn call(&mut self, inp: &[F]) -> Result<F, FlpError> {
        gadget_call_check(self, inp.len())?;
        Ok(poly_eval(&self.poly, inp[0]))
    }

    fn call_poly(&mut self, outp: &mut [F], inp: &[Vec<F>]) -> Result<(), FlpError> {
        gadget_call_poly_check(self, outp, inp)?;

        for item in outp.iter_mut() {
            *item = F::zero();
        }

        if inp[0].len() >= FFT_THRESHOLD {
            self.call_poly_fft(outp, inp)
        } else {
            self.call_poly_direct(outp, inp)
        }
    }

    fn arity(&self) -> usize {
        1
    }

    fn degree(&self) -> usize {
        poly_deg(&self.poly)
    }

    fn calls(&self) -> usize {
        self.num_calls
    }

    fn as_any(&mut self) -> &mut dyn Any {
        self
    }
}

/// Trait for abstracting over [`ParallelSum`].
pub trait ParallelSumGadget<F: FftFriendlyFieldElement, G>: Gadget<F> + Debug {
    /// Wraps `inner` into a sum gadget that calls it `chunks` many times, and adds the reuslts.
    fn new(inner: G, chunks: usize) -> Self;
}

/// A wrapper gadget that applies the inner gadget to chunks of input and returns the sum of the
/// outputs. The arity is equal to the arity of the inner gadget times the number of times it is
/// called.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ParallelSum<F: FftFriendlyFieldElement, G: Gadget<F>> {
    inner: G,
    chunks: usize,
    phantom: PhantomData<F>,
}

impl<F: FftFriendlyFieldElement, G: 'static + Gadget<F>> ParallelSumGadget<F, G>
    for ParallelSum<F, G>
{
    fn new(inner: G, chunks: usize) -> Self {
        Self {
            inner,
            chunks,
            phantom: PhantomData,
        }
    }
}

impl<F: FftFriendlyFieldElement, G: 'static + Gadget<F>> Gadget<F> for ParallelSum<F, G> {
    fn call(&mut self, inp: &[F]) -> Result<F, FlpError> {
        gadget_call_check(self, inp.len())?;
        let mut outp = F::zero();
        for chunk in inp.chunks(self.inner.arity()) {
            outp += self.inner.call(chunk)?;
        }
        Ok(outp)
    }

    fn call_poly(&mut self, outp: &mut [F], inp: &[Vec<F>]) -> Result<(), FlpError> {
        gadget_call_poly_check(self, outp, inp)?;

        for x in outp.iter_mut() {
            *x = F::zero();
        }

        let mut partial_outp = vec![F::zero(); outp.len()];

        for chunk in inp.chunks(self.inner.arity()) {
            self.inner.call_poly(&mut partial_outp, chunk)?;
            for i in 0..outp.len() {
                outp[i] += partial_outp[i]
            }
        }

        Ok(())
    }

    fn arity(&self) -> usize {
        self.chunks * self.inner.arity()
    }

    fn degree(&self) -> usize {
        self.inner.degree()
    }

    fn calls(&self) -> usize {
        self.inner.calls()
    }

    fn as_any(&mut self) -> &mut dyn Any {
        self
    }
}

/// A wrapper gadget that applies the inner gadget to chunks of input and returns the sum of the
/// outputs. The arity is equal to the arity of the inner gadget times the number of chunks. The sum
/// evaluation is multithreaded.
#[cfg(feature = "multithreaded")]
#[cfg_attr(docsrs, doc(cfg(feature = "multithreaded")))]
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ParallelSumMultithreaded<F: FftFriendlyFieldElement, G: Gadget<F>> {
    serial_sum: ParallelSum<F, G>,
}

#[cfg(feature = "multithreaded")]
impl<F, G> ParallelSumGadget<F, G> for ParallelSumMultithreaded<F, G>
where
    F: FftFriendlyFieldElement + Sync + Send,
    G: 'static + Gadget<F> + Clone + Sync + Send,
{
    fn new(inner: G, chunks: usize) -> Self {
        Self {
            serial_sum: ParallelSum::new(inner, chunks),
        }
    }
}

/// Data structures passed between fold operations in [`ParallelSumMultithreaded`].
#[cfg(feature = "multithreaded")]
struct ParallelSumFoldState<F, G> {
    /// Inner gadget.
    inner: G,
    /// Output buffer for `call_poly()`.
    partial_output: Vec<F>,
    /// Sum accumulator.
    partial_sum: Vec<F>,
}

#[cfg(feature = "multithreaded")]
impl<F, G> ParallelSumFoldState<F, G> {
    fn new(gadget: &G, length: usize) -> ParallelSumFoldState<F, G>
    where
        G: Clone,
        F: FftFriendlyFieldElement,
    {
        ParallelSumFoldState {
            inner: gadget.clone(),
            partial_output: vec![F::zero(); length],
            partial_sum: vec![F::zero(); length],
        }
    }
}

#[cfg(feature = "multithreaded")]
impl<F, G> Gadget<F> for ParallelSumMultithreaded<F, G>
where
    F: FftFriendlyFieldElement + Sync + Send,
    G: 'static + Gadget<F> + Clone + Sync + Send,
{
    fn call(&mut self, inp: &[F]) -> Result<F, FlpError> {
        self.serial_sum.call(inp)
    }

    fn call_poly(&mut self, outp: &mut [F], inp: &[Vec<F>]) -> Result<(), FlpError> {
        gadget_call_poly_check(self, outp, inp)?;

        let res = inp
            .par_chunks(self.serial_sum.inner.arity())
            .fold(
                || ParallelSumFoldState::new(&self.serial_sum.inner, outp.len()),
                |mut state, chunk| {
                    state
                        .inner
                        .call_poly(&mut state.partial_output, chunk)
                        .unwrap();
                    for (sum_elem, output_elem) in state
                        .partial_sum
                        .iter_mut()
                        .zip(state.partial_output.iter())
                    {
                        *sum_elem += *output_elem;
                    }
                    state
                },
            )
            .map(|state| state.partial_sum)
            .reduce(
                || vec![F::zero(); outp.len()],
                |mut x, y| {
                    for (xi, yi) in x.iter_mut().zip(y.iter()) {
                        *xi += *yi;
                    }
                    x
                },
            );

        outp.copy_from_slice(&res[..]);
        Ok(())
    }

    fn arity(&self) -> usize {
        self.serial_sum.arity()
    }

    fn degree(&self) -> usize {
        self.serial_sum.degree()
    }

    fn calls(&self) -> usize {
        self.serial_sum.calls()
    }

    fn as_any(&mut self) -> &mut dyn Any {
        self
    }
}

fn gadget_call_check<F: FftFriendlyFieldElement, G: Gadget<F>>(
    gadget: &G,
    in_len: usize,
) -> Result<(), FlpError> {
    if in_len != gadget.arity() {
        return Err(FlpError::Gadget(format!(
            "unexpected number of inputs: got {}; want {}",
            in_len,
            gadget.arity()
        )));
    }

    if in_len == 0 {
        return Err(FlpError::Gadget("can't call an arity-0 gadget".to_string()));
    }

    Ok(())
}

fn gadget_call_poly_check<F: FftFriendlyFieldElement, G: Gadget<F>>(
    gadget: &G,
    outp: &[F],
    inp: &[Vec<F>],
) -> Result<(), FlpError>
where
    G: Gadget<F>,
{
    gadget_call_check(gadget, inp.len())?;

    for i in 1..inp.len() {
        if inp[i].len() != inp[0].len() {
            return Err(FlpError::Gadget(
                "gadget called on wire polynomials with different lengths".to_string(),
            ));
        }
    }

    let expected = gadget_poly_len(gadget.degree(), inp[0].len()).next_power_of_two();
    if outp.len() != expected {
        return Err(FlpError::Gadget(format!(
            "incorrect output length: got {}; want {}",
            outp.len(),
            expected
        )));
    }

    Ok(())
}

#[inline]
fn gadget_poly_fft_mem_len(degree: usize, num_calls: usize) -> usize {
    gadget_poly_len(degree, wire_poly_len(num_calls)).next_power_of_two()
}
