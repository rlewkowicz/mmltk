// SPDX-License-Identifier: MPL-2.0

//! A collection of [`Type`] implementations.

use crate::field::{FftFriendlyFieldElement, FieldElementWithIntegerExt};
use crate::flp::gadgets::{Mul, ParallelSumGadget, PolyEval};
use crate::flp::{FlpError, Gadget, Type};
use crate::polynomial::poly_range_check;
use std::convert::TryInto;
use std::fmt::{self, Debug};
use std::marker::PhantomData;
use subtle::Choice;
/// The counter data type. Each measurement is `0` or `1` and the aggregate result is the sum of the measurements (i.e., the total number of `1s`).
#[derive(Clone, PartialEq, Eq)]
pub struct Count<F> {
    range_checker: Vec<F>,
}

impl<F> Debug for Count<F> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Count").finish()
    }
}

impl<F: FftFriendlyFieldElement> Count<F> {
    /// Return a new [`Count`] type instance.
    pub fn new() -> Self {
        Self {
            range_checker: poly_range_check(0, 2),
        }
    }
}

impl<F: FftFriendlyFieldElement> Default for Count<F> {
    fn default() -> Self {
        Self::new()
    }
}

impl<F: FftFriendlyFieldElement> Type for Count<F> {
    type Measurement = bool;
    type AggregateResult = F::Integer;
    type Field = F;

    fn encode_measurement(&self, value: &bool) -> Result<Vec<F>, FlpError> {
        Ok(vec![F::conditional_select(
            &F::zero(),
            &F::one(),
            Choice::from(u8::from(*value)),
        )])
    }

    fn decode_result(&self, data: &[F], _num_measurements: usize) -> Result<F::Integer, FlpError> {
        decode_result(data)
    }

    fn gadget(&self) -> Vec<Box<dyn Gadget<F>>> {
        vec![Box::new(Mul::new(1))]
    }

    fn valid(
        &self,
        g: &mut Vec<Box<dyn Gadget<F>>>,
        input: &[F],
        joint_rand: &[F],
        _num_shares: usize,
    ) -> Result<F, FlpError> {
        self.valid_call_check(input, joint_rand)?;
        Ok(g[0].call(&[input[0], input[0]])? - input[0])
    }

    fn truncate(&self, input: Vec<F>) -> Result<Vec<F>, FlpError> {
        self.truncate_call_check(&input)?;
        Ok(input)
    }

    fn input_len(&self) -> usize {
        1
    }

    fn proof_len(&self) -> usize {
        5
    }

    fn verifier_len(&self) -> usize {
        4
    }

    fn output_len(&self) -> usize {
        self.input_len()
    }

    fn joint_rand_len(&self) -> usize {
        0
    }

    fn prove_rand_len(&self) -> usize {
        2
    }

    fn query_rand_len(&self) -> usize {
        1
    }
}

/// This sum type. Each measurement is a integer in `[0, 2^bits)` and the aggregate is the sum of
/// the measurements.
///
/// The validity circuit is based on the SIMD circuit construction of [[BBCG+19], Theorem 5.3].
///
/// [BBCG+19]: https://ia.cr/2019/188
#[derive(Clone, PartialEq, Eq)]
pub struct Sum<F: FftFriendlyFieldElement> {
    bits: usize,
    range_checker: Vec<F>,
}

impl<F: FftFriendlyFieldElement> Debug for Sum<F> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Sum").field("bits", &self.bits).finish()
    }
}

impl<F: FftFriendlyFieldElement> Sum<F> {
    /// Return a new [`Sum`] type parameter. Each value of this type is an integer in range `[0,
    /// 2^bits)`.
    pub fn new(bits: usize) -> Result<Self, FlpError> {
        if !F::valid_integer_bitlength(bits) {
            return Err(FlpError::Encode(
                "invalid bits: number of bits exceeds maximum number of bits in this field"
                    .to_string(),
            ));
        }
        Ok(Self {
            bits,
            range_checker: poly_range_check(0, 2),
        })
    }
}

impl<F: FftFriendlyFieldElement> Type for Sum<F> {
    type Measurement = F::Integer;
    type AggregateResult = F::Integer;
    type Field = F;

    fn encode_measurement(&self, summand: &F::Integer) -> Result<Vec<F>, FlpError> {
        let v = F::encode_as_bitvector(*summand, self.bits)?.collect();
        Ok(v)
    }

    fn decode_result(&self, data: &[F], _num_measurements: usize) -> Result<F::Integer, FlpError> {
        decode_result(data)
    }

    fn gadget(&self) -> Vec<Box<dyn Gadget<F>>> {
        vec![Box::new(PolyEval::new(
            self.range_checker.clone(),
            self.bits,
        ))]
    }

    fn valid(
        &self,
        g: &mut Vec<Box<dyn Gadget<F>>>,
        input: &[F],
        joint_rand: &[F],
        _num_shares: usize,
    ) -> Result<F, FlpError> {
        self.valid_call_check(input, joint_rand)?;
        call_gadget_on_vec_entries(&mut g[0], input, joint_rand[0])
    }

    fn truncate(&self, input: Vec<F>) -> Result<Vec<F>, FlpError> {
        self.truncate_call_check(&input)?;
        let res = F::decode_bitvector(&input)?;
        Ok(vec![res])
    }

    fn input_len(&self) -> usize {
        self.bits
    }

    fn proof_len(&self) -> usize {
        2 * ((1 + self.bits).next_power_of_two() - 1) + 2
    }

    fn verifier_len(&self) -> usize {
        3
    }

    fn output_len(&self) -> usize {
        1
    }

    fn joint_rand_len(&self) -> usize {
        1
    }

    fn prove_rand_len(&self) -> usize {
        1
    }

    fn query_rand_len(&self) -> usize {
        1
    }
}

/// The average type. Each measurement is an integer in `[0,2^bits)` for some `0 < bits < 64` and the
/// aggregate is the arithmetic average.
#[derive(Clone, PartialEq, Eq)]
pub struct Average<F: FftFriendlyFieldElement> {
    bits: usize,
    range_checker: Vec<F>,
}

impl<F: FftFriendlyFieldElement> Debug for Average<F> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Average").field("bits", &self.bits).finish()
    }
}

impl<F: FftFriendlyFieldElement> Average<F> {
    /// Return a new [`Average`] type parameter. Each value of this type is an integer in range `[0,
    /// 2^bits)`.
    pub fn new(bits: usize) -> Result<Self, FlpError> {
        if !F::valid_integer_bitlength(bits) {
            return Err(FlpError::Encode(
                "invalid bits: number of bits exceeds maximum number of bits in this field"
                    .to_string(),
            ));
        }
        Ok(Self {
            bits,
            range_checker: poly_range_check(0, 2),
        })
    }
}

impl<F: FftFriendlyFieldElement> Type for Average<F> {
    type Measurement = F::Integer;
    type AggregateResult = f64;
    type Field = F;

    fn encode_measurement(&self, summand: &F::Integer) -> Result<Vec<F>, FlpError> {
        let v = F::encode_as_bitvector(*summand, self.bits)?.collect();
        Ok(v)
    }

    fn decode_result(&self, data: &[F], num_measurements: usize) -> Result<f64, FlpError> {
        let data = decode_result(data)?;
        let data: u64 = data.try_into().map_err(|err| {
            FlpError::Decode(format!("failed to convert {data:?} to u64: {err}",))
        })?;
        let result = (data as f64) / (num_measurements as f64);
        Ok(result)
    }

    fn gadget(&self) -> Vec<Box<dyn Gadget<F>>> {
        vec![Box::new(PolyEval::new(
            self.range_checker.clone(),
            self.bits,
        ))]
    }

    fn valid(
        &self,
        g: &mut Vec<Box<dyn Gadget<F>>>,
        input: &[F],
        joint_rand: &[F],
        _num_shares: usize,
    ) -> Result<F, FlpError> {
        self.valid_call_check(input, joint_rand)?;
        call_gadget_on_vec_entries(&mut g[0], input, joint_rand[0])
    }

    fn truncate(&self, input: Vec<F>) -> Result<Vec<F>, FlpError> {
        self.truncate_call_check(&input)?;
        let res = F::decode_bitvector(&input)?;
        Ok(vec![res])
    }

    fn input_len(&self) -> usize {
        self.bits
    }

    fn proof_len(&self) -> usize {
        2 * ((1 + self.bits).next_power_of_two() - 1) + 2
    }

    fn verifier_len(&self) -> usize {
        3
    }

    fn output_len(&self) -> usize {
        1
    }

    fn joint_rand_len(&self) -> usize {
        1
    }

    fn prove_rand_len(&self) -> usize {
        1
    }

    fn query_rand_len(&self) -> usize {
        1
    }
}

/// The histogram type. Each measurement is an integer in `[0, length)` and the aggregate is a
/// histogram counting the number of occurrences of each measurement.
#[derive(PartialEq, Eq)]
pub struct Histogram<F, S> {
    length: usize,
    chunk_length: usize,
    gadget_calls: usize,
    phantom: PhantomData<(F, S)>,
}

impl<F: FftFriendlyFieldElement, S> Debug for Histogram<F, S> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Histogram")
            .field("length", &self.length)
            .field("chunk_length", &self.chunk_length)
            .finish()
    }
}

impl<F: FftFriendlyFieldElement, S: ParallelSumGadget<F, Mul<F>>> Histogram<F, S> {
    /// Return a new [`Histogram`] type with the given number of buckets.
    pub fn new(length: usize, chunk_length: usize) -> Result<Self, FlpError> {
        if length >= u32::MAX as usize {
            return Err(FlpError::Encode(
                "invalid length: number of buckets exceeds maximum permitted".to_string(),
            ));
        }
        if length == 0 {
            return Err(FlpError::InvalidParameter(
                "length cannot be zero".to_string(),
            ));
        }
        if chunk_length == 0 {
            return Err(FlpError::InvalidParameter(
                "chunk_length cannot be zero".to_string(),
            ));
        }

        let mut gadget_calls = length / chunk_length;
        if length % chunk_length != 0 {
            gadget_calls += 1;
        }

        Ok(Self {
            length,
            chunk_length,
            gadget_calls,
            phantom: PhantomData,
        })
    }
}

impl<F, S> Clone for Histogram<F, S> {
    fn clone(&self) -> Self {
        Self {
            length: self.length,
            chunk_length: self.chunk_length,
            gadget_calls: self.gadget_calls,
            phantom: self.phantom,
        }
    }
}

impl<F, S> Type for Histogram<F, S>
where
    F: FftFriendlyFieldElement,
    S: ParallelSumGadget<F, Mul<F>> + Eq + 'static,
{
    type Measurement = usize;
    type AggregateResult = Vec<F::Integer>;
    type Field = F;

    fn encode_measurement(&self, measurement: &usize) -> Result<Vec<F>, FlpError> {
        let mut data = vec![F::zero(); self.length];

        data[*measurement] = F::one();
        Ok(data)
    }

    fn decode_result(
        &self,
        data: &[F],
        _num_measurements: usize,
    ) -> Result<Vec<F::Integer>, FlpError> {
        decode_result_vec(data, self.length)
    }

    fn gadget(&self) -> Vec<Box<dyn Gadget<F>>> {
        vec![Box::new(S::new(
            Mul::new(self.gadget_calls),
            self.chunk_length,
        ))]
    }

    fn valid(
        &self,
        g: &mut Vec<Box<dyn Gadget<F>>>,
        input: &[F],
        joint_rand: &[F],
        num_shares: usize,
    ) -> Result<F, FlpError> {
        self.valid_call_check(input, joint_rand)?;

        let range_check = parallel_sum_range_checks(
            &mut g[0],
            input,
            joint_rand[0],
            self.chunk_length,
            num_shares,
        )?;

        let mut sum_check = -(F::one() / F::from(F::valid_integer_try_from(num_shares)?));
        for val in input.iter() {
            sum_check += *val;
        }

        let out = joint_rand[1] * range_check + (joint_rand[1] * joint_rand[1]) * sum_check;
        Ok(out)
    }

    fn truncate(&self, input: Vec<F>) -> Result<Vec<F>, FlpError> {
        self.truncate_call_check(&input)?;
        Ok(input)
    }

    fn input_len(&self) -> usize {
        self.length
    }

    fn proof_len(&self) -> usize {
        (self.chunk_length * 2) + 2 * ((1 + self.gadget_calls).next_power_of_two() - 1) + 1
    }

    fn verifier_len(&self) -> usize {
        2 + self.chunk_length * 2
    }

    fn output_len(&self) -> usize {
        self.input_len()
    }

    fn joint_rand_len(&self) -> usize {
        2
    }

    fn prove_rand_len(&self) -> usize {
        self.chunk_length * 2
    }

    fn query_rand_len(&self) -> usize {
        1
    }
}

/// A sequence of integers in range `[0, 2^bits)`. This type uses a neat trick from [[BBCG+19],
/// Corollary 4.9] to reduce the proof size to roughly the square root of the input size.
///
/// [BBCG+19]: https://eprint.iacr.org/2019/188
#[derive(PartialEq, Eq)]
pub struct SumVec<F: FftFriendlyFieldElement, S> {
    len: usize,
    bits: usize,
    flattened_len: usize,
    max: F::Integer,
    chunk_length: usize,
    gadget_calls: usize,
    phantom: PhantomData<S>,
}

impl<F: FftFriendlyFieldElement, S> Debug for SumVec<F, S> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SumVec")
            .field("len", &self.len)
            .field("bits", &self.bits)
            .field("chunk_length", &self.chunk_length)
            .finish()
    }
}

impl<F: FftFriendlyFieldElement, S: ParallelSumGadget<F, Mul<F>>> SumVec<F, S> {
    /// Returns a new [`SumVec`] with the desired bit width and vector length.
    ///
    /// # Errors
    ///
    /// * The length of the encoded measurement, i.e., `bits * len`, overflows addressable memory.
    /// * The bit width cannot be encoded, i.e., `bits` is larger than or equal to the number of
    ///   bits required to encode field elements.
    /// * Any of `bits`, `len`, or `chunk_length` are zero.
    pub fn new(bits: usize, len: usize, chunk_length: usize) -> Result<Self, FlpError> {
        let flattened_len = bits.checked_mul(len).ok_or_else(|| {
            FlpError::InvalidParameter("`bits*len` overflows addressable memory".into())
        })?;

        let limit = std::mem::size_of::<F::Integer>() * 8 - 1;
        if bits > limit {
            return Err(FlpError::InvalidParameter(format!(
                "bit wdith exceeds limit of {limit}"
            )));
        }

        if bits == 0 {
            return Err(FlpError::InvalidParameter(
                "bits cannot be zero".to_string(),
            ));
        }
        if len == 0 {
            return Err(FlpError::InvalidParameter("len cannot be zero".to_string()));
        }
        if chunk_length == 0 {
            return Err(FlpError::InvalidParameter(
                "chunk_length cannot be zero".to_string(),
            ));
        }

        let one = F::Integer::from(F::one());
        let max = (one << bits) - one;

        let mut gadget_calls = flattened_len / chunk_length;
        if flattened_len % chunk_length != 0 {
            gadget_calls += 1;
        }

        Ok(Self {
            len,
            bits,
            flattened_len,
            max,
            chunk_length,
            gadget_calls,
            phantom: PhantomData,
        })
    }
}

impl<F: FftFriendlyFieldElement, S> Clone for SumVec<F, S> {
    fn clone(&self) -> Self {
        Self {
            len: self.len,
            bits: self.bits,
            flattened_len: self.flattened_len,
            max: self.max,
            chunk_length: self.chunk_length,
            gadget_calls: self.gadget_calls,
            phantom: PhantomData,
        }
    }
}

impl<F, S> Type for SumVec<F, S>
where
    F: FftFriendlyFieldElement,
    S: ParallelSumGadget<F, Mul<F>> + Eq + 'static,
{
    type Measurement = Vec<F::Integer>;
    type AggregateResult = Vec<F::Integer>;
    type Field = F;

    fn encode_measurement(&self, measurement: &Vec<F::Integer>) -> Result<Vec<F>, FlpError> {
        if measurement.len() != self.len {
            return Err(FlpError::Encode(format!(
                "unexpected measurement length: got {}; want {}",
                measurement.len(),
                self.len
            )));
        }

        let mut flattened = Vec::with_capacity(self.flattened_len);
        for summand in measurement.iter() {
            if summand > &self.max {
                return Err(FlpError::Encode(format!(
                    "summand exceeds maximum of 2^{}-1",
                    self.bits
                )));
            }
            flattened.extend(F::encode_as_bitvector(*summand, self.bits)?);
        }

        Ok(flattened)
    }

    fn decode_result(
        &self,
        data: &[F],
        _num_measurements: usize,
    ) -> Result<Vec<F::Integer>, FlpError> {
        decode_result_vec(data, self.len)
    }

    fn gadget(&self) -> Vec<Box<dyn Gadget<F>>> {
        vec![Box::new(S::new(
            Mul::new(self.gadget_calls),
            self.chunk_length,
        ))]
    }

    fn valid(
        &self,
        g: &mut Vec<Box<dyn Gadget<F>>>,
        input: &[F],
        joint_rand: &[F],
        num_shares: usize,
    ) -> Result<F, FlpError> {
        self.valid_call_check(input, joint_rand)?;

        parallel_sum_range_checks(
            &mut g[0],
            input,
            joint_rand[0],
            self.chunk_length,
            num_shares,
        )
    }

    fn truncate(&self, input: Vec<F>) -> Result<Vec<F>, FlpError> {
        self.truncate_call_check(&input)?;
        let mut unflattened = Vec::with_capacity(self.len);
        for chunk in input.chunks(self.bits) {
            unflattened.push(F::decode_bitvector(chunk)?);
        }
        Ok(unflattened)
    }

    fn input_len(&self) -> usize {
        self.flattened_len
    }

    fn proof_len(&self) -> usize {
        (self.chunk_length * 2) + 2 * ((1 + self.gadget_calls).next_power_of_two() - 1) + 1
    }

    fn verifier_len(&self) -> usize {
        2 + self.chunk_length * 2
    }

    fn output_len(&self) -> usize {
        self.len
    }

    fn joint_rand_len(&self) -> usize {
        1
    }

    fn prove_rand_len(&self) -> usize {
        self.chunk_length * 2
    }

    fn query_rand_len(&self) -> usize {
        1
    }
}

/// Compute a random linear combination of the result of calls of `g` on each element of `input`.
///
/// # Arguments
///
/// * `g` - The gadget to be applied elementwise
/// * `input` - The vector on whose elements to apply `g`
/// * `rnd` - The randomness used for the linear combination
pub(crate) fn call_gadget_on_vec_entries<F: FftFriendlyFieldElement>(
    g: &mut Box<dyn Gadget<F>>,
    input: &[F],
    rnd: F,
) -> Result<F, FlpError> {
    let mut range_check = F::zero();
    let mut r = rnd;
    for chunk in input.chunks(1) {
        range_check += r * g.call(chunk)?;
        r *= rnd;
    }
    Ok(range_check)
}

/// Given a vector `data` of field elements which should contain exactly one entry, return the
/// integer representation of that entry.
pub(crate) fn decode_result<F: FftFriendlyFieldElement>(
    data: &[F],
) -> Result<F::Integer, FlpError> {
    if data.len() != 1 {
        return Err(FlpError::Decode("unexpected input length".into()));
    }
    Ok(F::Integer::from(data[0]))
}

/// Given a vector `data` of field elements, return a vector containing the corresponding integer
/// representations, if the number of entries matches `expected_len`.
pub(crate) fn decode_result_vec<F: FftFriendlyFieldElement>(
    data: &[F],
    expected_len: usize,
) -> Result<Vec<F::Integer>, FlpError> {
    if data.len() != expected_len {
        return Err(FlpError::Decode("unexpected input length".into()));
    }
    Ok(data.iter().map(|elem| F::Integer::from(*elem)).collect())
}

/// This evaluates range checks on a slice of field elements, using a ParallelSum gadget evaluating
/// many multiplication gates.
///
/// # Arguments
///
/// * `gadget`: A `ParallelSumGadget<F, Mul<F>>` gadget, or a shim wrapping the same.
/// * `input`: A slice of inputs. This calculation will check that all inputs were zero or one
///   before secret sharing.
/// * `joint_randomness`: A joint randomness value, used to compute a random linear combination of
///   individual range checks.
/// * `chunk_length`: How many multiplication gates per ParallelSum gadget. This must match what the
///   gadget was constructed with.
/// * `num_shares`: The number of shares that the inputs were secret shared into. This is needed to
///   correct constant terms in the circuit.
///
/// # Returns
///
/// This returns (additive shares of) zero if all inputs were zero or one, and otherwise returns a
/// non-zero value with high probability.
pub(crate) fn parallel_sum_range_checks<F: FftFriendlyFieldElement>(
    gadget: &mut Box<dyn Gadget<F>>,
    input: &[F],
    joint_randomness: F,
    chunk_length: usize,
    num_shares: usize,
) -> Result<F, FlpError> {
    let f_num_shares = F::from(F::valid_integer_try_from::<usize>(num_shares)?);
    let num_shares_inverse = f_num_shares.inv();

    let mut output = F::zero();
    let mut r_power = joint_randomness;
    let mut padded_chunk = vec![F::zero(); 2 * chunk_length];

    for chunk in input.chunks(chunk_length) {
        for (input, args) in chunk.iter().zip(padded_chunk.chunks_exact_mut(2)) {
            args[0] = r_power * *input;
            args[1] = *input - num_shares_inverse;
            r_power *= joint_randomness;
        }
        for args in padded_chunk[chunk.len() * 2..].chunks_exact_mut(2) {
            args[0] = F::zero();
            args[1] = -num_shares_inverse;
        }

        output += gadget.call(&padded_chunk)?;
    }

    Ok(output)
}


#[cfg(feature = "experimental")]
#[cfg_attr(docsrs, doc(cfg(feature = "experimental")))]
pub mod fixedpoint_l2;
