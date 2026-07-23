// Copyright (c) 2020 Apple Inc.
// SPDX-License-Identifier: MPL-2.0

//! Finite field arithmetic.
//!
//! Basic field arithmetic is captured in the [`FieldElement`] trait. Fields used in Prio implement
//! [`FftFriendlyFieldElement`], and have an associated element called the "generator" that
//! generates a multiplicative subgroup of order `2^n` for some `n`.

use crate::prng::{Prng, PrngError};
use crate::{
    codec::{CodecError, Decode, Encode},
    fp::{FP128, FP32, FP64},
};
use serde::{
    de::{DeserializeOwned, Visitor},
    Deserialize, Deserializer, Serialize, Serializer,
};
use std::{
    cmp::min,
    convert::{TryFrom, TryInto},
    fmt::{self, Debug, Display, Formatter},
    hash::{Hash, Hasher},
    io::{Cursor, Read},
    marker::PhantomData,
    ops::{
        Add, AddAssign, BitAnd, ControlFlow, Div, DivAssign, Mul, MulAssign, Neg, Range, Shl, Shr,
        Sub, SubAssign,
    },
};
use subtle::{Choice, ConditionallyNegatable, ConditionallySelectable, ConstantTimeEq};

#[cfg(feature = "experimental")]
mod field255;

#[cfg(feature = "experimental")]
pub use field255::Field255;

/// Possible errors from finite field operations.
#[derive(Debug, thiserror::Error)]
#[non_exhaustive]
pub enum FieldError {
    /// Input sizes do not match.
    #[error("input sizes do not match")]
    InputSizeMismatch,
    /// Returned when decoding a [`FieldElement`] from a too-short byte string.
    #[error("short read from bytes")]
    ShortRead,
    /// Returned when converting an integer to a [`FieldElement`] if the integer is greater than or
    /// equal to the field modulus.
    #[error("input value exceeds modulus")]
    ModulusOverflow,
    /// Error while performing I/O.
    #[error("I/O error")]
    Io(#[from] std::io::Error),
    /// Error encoding or decoding a field.
    #[error("Codec error")]
    #[deprecated]
    Codec(CodecError),
    /// Error converting to [`FieldElementWithInteger::Integer`].
    #[error("Integer TryFrom error")]
    IntegerTryFrom,
    /// Returned when encoding an integer to "bitvector representation", or decoding from the same,
    /// if the number of bits is larger than the bit length of the field's modulus.
    #[error("bit vector length exceeds modulus bit length")]
    BitVectorTooLong,
}

/// Objects with this trait represent an element of `GF(p)` for some prime `p`.
pub trait FieldElement:
    Sized
    + Debug
    + Copy
    + PartialEq
    + Eq
    + ConstantTimeEq
    + ConditionallySelectable
    + ConditionallyNegatable
    + Add<Output = Self>
    + AddAssign
    + Sub<Output = Self>
    + SubAssign
    + Mul<Output = Self>
    + MulAssign
    + Div<Output = Self>
    + DivAssign
    + Neg<Output = Self>
    + Display
    + for<'a> TryFrom<&'a [u8], Error = FieldError>
    + Into<Vec<u8>>
    + Serialize
    + DeserializeOwned
    + Encode
    + Decode
    + 'static 
{
    /// Size in bytes of an encoded field element.
    const ENCODED_SIZE: usize;

    /// Modular inversion, i.e., `self^-1 (mod p)`. If `self` is 0, then the output is undefined.
    fn inv(&self) -> Self;

    /// Interprets the next [`Self::ENCODED_SIZE`] bytes from the input slice as an element of the
    /// field. Any of the most significant bits beyond the bit length of the modulus will be
    /// cleared, in order to minimize the amount of rejection sampling needed.
    ///
    /// # Errors
    ///
    /// An error is returned if the provided slice is too small to encode a field element or if the
    /// result encodes an integer larger than or equal to the field modulus.
    ///
    /// # Warnings
    ///
    /// This function should only be used internally to convert a random byte string into
    /// a field element. Use [`Decode::decode`] to deserialize field elements. Use
    /// [`random_vector`] to randomly generate field elements.
    #[doc(hidden)]
    fn try_from_random(bytes: &[u8]) -> Result<Self, FieldError>;

    /// Returns the additive identity.
    fn zero() -> Self;

    /// Returns the multiplicative identity.
    fn one() -> Self;

    /// Convert a slice of field elements into a vector of bytes.
    ///
    /// # Notes
    ///
    /// Ideally we would implement `From<&[F: FieldElement]> for Vec<u8>` or the corresponding
    /// `Into`, but the orphan rule and the stdlib's blanket implementations of `Into` make this
    /// impossible.
    #[deprecated]
    fn slice_into_byte_vec(values: &[Self]) -> Vec<u8> {
        let mut vec = Vec::with_capacity(values.len() * Self::ENCODED_SIZE);
        encode_fieldvec(values, &mut vec).unwrap();
        vec
    }

    /// Convert a slice of bytes into a vector of field elements. The slice is interpreted as a
    /// sequence of [`Self::ENCODED_SIZE`]-byte sequences.
    ///
    /// # Errors
    ///
    /// Returns an error if the length of the provided byte slice is not a multiple of the size of a
    /// field element, or if any of the values in the byte slice are invalid encodings of a field
    /// element, because the encoded integer is larger than or equal to the field modulus.
    ///
    /// # Notes
    ///
    /// Ideally we would implement `From<&[u8]> for Vec<F: FieldElement>` or the corresponding
    /// `Into`, but the orphan rule and the stdlib's blanket implementations of `Into` make this
    /// impossible.
    #[deprecated]
    fn byte_slice_into_vec(bytes: &[u8]) -> Result<Vec<Self>, FieldError> {
        if bytes.len() % Self::ENCODED_SIZE != 0 {
            return Err(FieldError::ShortRead);
        }
        let mut vec = Vec::with_capacity(bytes.len() / Self::ENCODED_SIZE);
        for chunk in bytes.chunks_exact(Self::ENCODED_SIZE) {
            #[allow(deprecated)]
            vec.push(Self::get_decoded(chunk).map_err(FieldError::Codec)?);
        }
        Ok(vec)
    }
}

/// An integer type that accompanies a finite field. Integers and field elements may be converted
/// back and forth via the natural map between residue classes modulo 'p' and integers between 0
/// and p - 1.
pub trait Integer:
    Debug
    + Eq
    + Ord
    + BitAnd<Output = Self>
    + Div<Output = Self>
    + Shl<usize, Output = Self>
    + Shr<usize, Output = Self>
    + Add<Output = Self>
    + Sub<Output = Self>
    + TryFrom<usize, Error = Self::TryFromUsizeError>
    + TryInto<u64, Error = Self::TryIntoU64Error>
{
    /// The error returned if converting `usize` to this integer type fails.
    type TryFromUsizeError: std::error::Error;

    /// The error returned if converting this integer type to a `u64` fails.
    type TryIntoU64Error: std::error::Error;

    /// Returns zero.
    fn zero() -> Self;

    /// Returns one.
    fn one() -> Self;
}

/// Extension trait for field elements that can be converted back and forth to an integer type.
///
/// The `Integer` associated type is an integer (primitive or otherwise) that supports various
/// arithmetic operations. The order of the field is guaranteed to fit inside the range of the
/// integer type. This trait also defines methods on field elements, `pow` and `modulus`, that make
/// use of the associated integer type.
pub trait FieldElementWithInteger: FieldElement + From<Self::Integer> {
    /// The integer representation of a field element.
    type Integer: Integer + From<Self> + Copy;

    /// Modular exponentation, i.e., `self^exp (mod p)`.
    fn pow(&self, exp: Self::Integer) -> Self;

    /// Returns the prime modulus `p`.
    fn modulus() -> Self::Integer;
    /// Encode the integer `input` as a sequence of bits in two's complement representation, least
    /// significant bit first, and then map each bit to a field element.
    ///
    /// Returns an error if `input` cannot be represented with `bits` many bits, or if `bits`
    /// is larger than the bit width of the field's modulus.
    fn encode_as_bitvector(
        input: Self::Integer,
        bits: usize,
    ) -> Result<BitvectorRepresentationIter<Self>, FieldError> {
        if !Self::valid_integer_bitlength(bits) {
            return Err(FieldError::BitVectorTooLong);
        }

        if input >> bits != Self::Integer::zero() {
            return Err(FieldError::InputSizeMismatch);
        }

        Ok(BitvectorRepresentationIter {
            inner: 0..bits,
            input,
        })
    }

    /// Inverts the encoding done by [`Self::encode_as_bitvector`], and returns a single field
    /// element.
    ///
    /// This performs an inner product between the input vector of field elements and successive
    /// powers of two (starting with 2^0 = 1). If the input came from [`Self::encode_as_bitvector`],
    /// then the result will be equal to the originally encoded integer, projected into the field.
    ///
    /// Note that this decoding operation is linear, so it can be applied to secret shares of an
    /// encoded integer, and if the results are summed up, it will be equal to the encoded integer.
    ///
    /// Returns an error if the length of the input is larger than the bit width of the field's
    /// modulus.
    fn decode_bitvector(input: &[Self]) -> Result<Self, FieldError> {
        if !Self::valid_integer_bitlength(input.len()) {
            return Err(FieldError::BitVectorTooLong);
        }

        let mut decoded = Self::zero();
        let one = Self::one();
        let two = one + one;
        let mut power_of_two = one;
        for value in input.iter() {
            decoded += *value * power_of_two;
            power_of_two *= two;
        }
        Ok(decoded)
    }
}

/// This iterator returns a sequence of field elements that are equal to zero or one, representing
/// some integer in two's complement form. See [`FieldElementWithInteger::encode_as_bitvector`].
#[derive(Debug, Clone)]
pub struct BitvectorRepresentationIter<F: FieldElementWithInteger> {
    inner: Range<usize>,
    input: F::Integer,
}

impl<F> Iterator for BitvectorRepresentationIter<F>
where
    F: FieldElementWithInteger,
{
    type Item = F;

    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        let bit_offset = self.inner.next()?;
        Some(F::from((self.input >> bit_offset) & F::Integer::one()))
    }
}

/// Methods common to all `FieldElementWithInteger` implementations that are private to the crate.
pub(crate) trait FieldElementWithIntegerExt: FieldElementWithInteger {
    /// Interpret `i` as [`Self::Integer`] if it's representable in that type and smaller than the
    /// field modulus.
    fn valid_integer_try_from<N>(i: N) -> Result<Self::Integer, FieldError>
    where
        Self::Integer: TryFrom<N>,
    {
        let i_int = Self::Integer::try_from(i).map_err(|_| FieldError::IntegerTryFrom)?;
        if Self::modulus() <= i_int {
            return Err(FieldError::ModulusOverflow);
        }
        Ok(i_int)
    }

    /// Check if the largest number representable with `bits` bits (i.e. 2^bits - 1) is
    /// representable in this field.
    fn valid_integer_bitlength(bits: usize) -> bool {
        if bits >= 8 * Self::ENCODED_SIZE {
            return false;
        }
        if Self::modulus() >> bits != Self::Integer::zero() {
            return true;
        }
        false
    }
}

impl<F: FieldElementWithInteger> FieldElementWithIntegerExt for F {}

/// Methods common to all `FieldElement` implementations that are private to the crate.
pub(crate) trait FieldElementExt: FieldElement {
    /// Try to interpret a slice of [`Self::ENCODED_SIZE`] random bytes as an element in the field. If
    /// the input represents an integer greater than or equal to the field modulus, then
    /// [`ControlFlow::Continue`] is returned instead, to indicate that an enclosing rejection sampling
    /// loop should try again with different random bytes.
    ///
    /// # Panics
    ///
    /// Panics if `bytes` is not of length [`Self::ENCODED_SIZE`].
    fn from_random_rejection(bytes: &[u8]) -> ControlFlow<Self, ()> {
        match Self::try_from_random(bytes) {
            Ok(x) => ControlFlow::Break(x),
            Err(FieldError::ModulusOverflow) => ControlFlow::Continue(()),
            Err(err) => panic!("unexpected error: {err}"),
        }
    }
}

impl<F: FieldElement> FieldElementExt for F {}

/// serde Visitor implementation used to generically deserialize `FieldElement`
/// values from byte arrays.
pub(crate) struct FieldElementVisitor<F: FieldElement> {
    pub(crate) phantom: PhantomData<F>,
}

impl<'de, F: FieldElement> Visitor<'de> for FieldElementVisitor<F> {
    type Value = F;

    fn expecting(&self, formatter: &mut Formatter) -> fmt::Result {
        formatter.write_fmt(format_args!("an array of {} bytes", F::ENCODED_SIZE))
    }

    fn visit_bytes<E>(self, v: &[u8]) -> Result<Self::Value, E>
    where
        E: serde::de::Error,
    {
        Self::Value::try_from(v).map_err(E::custom)
    }

    fn visit_seq<A>(self, mut seq: A) -> Result<Self::Value, A::Error>
    where
        A: serde::de::SeqAccess<'de>,
    {
        let mut bytes = vec![];
        while let Some(byte) = seq.next_element()? {
            bytes.push(byte);
        }

        self.visit_bytes(&bytes)
    }
}

/// Objects with this trait represent an element of `GF(p)`, where `p` is some prime and the
/// field's multiplicative group has a subgroup with an order that is a power of 2, and at least
/// `2^20`.
pub trait FftFriendlyFieldElement: FieldElementWithInteger {
    /// Returns the size of the multiplicative subgroup generated by
    /// [`FftFriendlyFieldElement::generator`].
    fn generator_order() -> Self::Integer;

    /// Returns the generator of the multiplicative subgroup of size
    /// [`FftFriendlyFieldElement::generator_order`].
    fn generator() -> Self;

    /// Returns the `2^l`-th principal root of unity for any `l <= 20`. Note that the `2^0`-th
    /// prinicpal root of unity is `1` by definition.
    fn root(l: usize) -> Option<Self>;
}

macro_rules! make_field {
    (
        $(#[$meta:meta])*
        $elem:ident, $int:ident, $fp:ident, $encoding_size:literal,
    ) => {
        $(#[$meta])*
        ///
        /// This structure represents a field element in a prime order field. The concrete
        /// representation of the element is via the Montgomery domain. For an element `n` in
        /// `GF(p)`, we store `n * R^-1 mod p` (where `R` is a given power of two). This
        /// representation enables using a more efficient (and branchless) multiplication algorithm,
        /// at the expense of having to convert elements between their Montgomery domain
        /// representation and natural representation. For calculations with many multiplications or
        /// exponentiations, this is worthwhile.
        ///
        /// As an invariant, this integer representing the field element in the Montgomery domain
        /// must be less than the field modulus, `p`.
        #[derive(Clone, Copy, Default)]
        pub struct $elem(u128);

        impl $elem {
            /// Attempts to instantiate an `$elem` from the first `Self::ENCODED_SIZE` bytes in the
            /// provided slice. The decoded value will be bitwise-ANDed with `mask` before reducing
            /// it using the field modulus.
            ///
            /// # Errors
            ///
            /// An error is returned if the provided slice is not long enough to encode a field
            /// element or if the decoded value is greater than the field prime.
            ///
            /// # Notes
            ///
            /// We cannot use `u128::from_le_bytes` or `u128::from_be_bytes` because those functions
            /// expect inputs to be exactly 16 bytes long. Our encoding of most field elements is
            /// more compact.
            fn try_from_bytes(bytes: &[u8], mask: u128) -> Result<Self, FieldError> {
                if Self::ENCODED_SIZE > bytes.len() {
                    return Err(FieldError::ShortRead);
                }

                let mut int = 0;
                for i in 0..Self::ENCODED_SIZE {
                    int |= (bytes[i] as u128) << (i << 3);
                }

                int &= mask;

                if int >= $fp.p {
                    return Err(FieldError::ModulusOverflow);
                }
                Ok(Self($fp.montgomery(int)))
            }
        }

        impl PartialEq for $elem {
            fn eq(&self, rhs: &Self) -> bool {

                debug_assert!(self.0 < $fp.p);
                debug_assert!(rhs.0 < $fp.p);

                self.0 == rhs.0
            }
        }

        impl ConstantTimeEq for $elem {
            fn ct_eq(&self, rhs: &Self) -> Choice {
                self.0.ct_eq(&rhs.0)
            }
        }

        impl ConditionallySelectable for $elem {
            fn conditional_select(a: &Self, b: &Self, choice: subtle::Choice) -> Self {
                Self(u128::conditional_select(&a.0, &b.0, choice))
            }
        }

        impl Hash for $elem {
            fn hash<H: Hasher>(&self, state: &mut H) {

                debug_assert!(self.0 < $fp.p);

                self.0.hash(state);
            }
        }

        impl Eq for $elem {}

        impl Add for $elem {
            type Output = $elem;
            fn add(self, rhs: Self) -> Self {
                Self($fp.add(self.0, rhs.0))
            }
        }

        impl Add for &$elem {
            type Output = $elem;
            fn add(self, rhs: Self) -> $elem {
                *self + *rhs
            }
        }

        impl AddAssign for $elem {
            fn add_assign(&mut self, rhs: Self) {
                *self = *self + rhs;
            }
        }

        impl Sub for $elem {
            type Output = $elem;
            fn sub(self, rhs: Self) -> Self {
                Self($fp.sub(self.0, rhs.0))
            }
        }

        impl Sub for &$elem {
            type Output = $elem;
            fn sub(self, rhs: Self) -> $elem {
                *self - *rhs
            }
        }

        impl SubAssign for $elem {
            fn sub_assign(&mut self, rhs: Self) {
                *self = *self - rhs;
            }
        }

        impl Mul for $elem {
            type Output = $elem;
            fn mul(self, rhs: Self) -> Self {
                Self($fp.mul(self.0, rhs.0))
            }
        }

        impl Mul for &$elem {
            type Output = $elem;
            fn mul(self, rhs: Self) -> $elem {
                *self * *rhs
            }
        }

        impl MulAssign for $elem {
            fn mul_assign(&mut self, rhs: Self) {
                *self = *self * rhs;
            }
        }

        impl Div for $elem {
            type Output = $elem;
            #[allow(clippy::suspicious_arithmetic_impl)]
            fn div(self, rhs: Self) -> Self {
                self * rhs.inv()
            }
        }

        impl Div for &$elem {
            type Output = $elem;
            fn div(self, rhs: Self) -> $elem {
                *self / *rhs
            }
        }

        impl DivAssign for $elem {
            fn div_assign(&mut self, rhs: Self) {
                *self = *self / rhs;
            }
        }

        impl Neg for $elem {
            type Output = $elem;
            fn neg(self) -> Self {
                Self($fp.neg(self.0))
            }
        }

        impl Neg for &$elem {
            type Output = $elem;
            fn neg(self) -> $elem {
                -(*self)
            }
        }

        impl From<$int> for $elem {
            fn from(x: $int) -> Self {
                Self($fp.montgomery(u128::try_from(x).unwrap()))
            }
        }

        impl From<$elem> for $int {
            fn from(x: $elem) -> Self {
                $int::try_from($fp.residue(x.0)).unwrap()
            }
        }

        impl PartialEq<$int> for $elem {
            fn eq(&self, rhs: &$int) -> bool {
                $fp.residue(self.0) == u128::try_from(*rhs).unwrap()
            }
        }

        impl<'a> TryFrom<&'a [u8]> for $elem {
            type Error = FieldError;

            fn try_from(bytes: &[u8]) -> Result<Self, FieldError> {
                Self::try_from_bytes(bytes, u128::MAX)
            }
        }

        impl From<$elem> for [u8; $elem::ENCODED_SIZE] {
            fn from(elem: $elem) -> Self {
                let int = $fp.residue(elem.0);
                let mut slice = [0; $elem::ENCODED_SIZE];
                for i in 0..$elem::ENCODED_SIZE {
                    slice[i] = ((int >> (i << 3)) & 0xff) as u8;
                }
                slice
            }
        }

        impl From<$elem> for Vec<u8> {
            fn from(elem: $elem) -> Self {
                <[u8; $elem::ENCODED_SIZE]>::from(elem).to_vec()
            }
        }

        impl Display for $elem {
            fn fmt(&self, f: &mut Formatter) -> std::fmt::Result {
                write!(f, "{}", $fp.residue(self.0))
            }
        }

        impl Debug for $elem {
            fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
                write!(f, "{}", $fp.residue(self.0))
            }
        }

        impl Serialize for $elem {
            fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
                let bytes: [u8; $elem::ENCODED_SIZE] = (*self).into();
                serializer.serialize_bytes(&bytes)
            }
        }

        impl<'de> Deserialize<'de> for $elem {
            fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<$elem, D::Error> {
                deserializer.deserialize_bytes(FieldElementVisitor { phantom: PhantomData })
            }
        }

        impl Encode for $elem {
            fn encode(&self, bytes: &mut Vec<u8>) -> Result<(), CodecError> {
                let slice = <[u8; $elem::ENCODED_SIZE]>::from(*self);
                bytes.extend_from_slice(&slice);
                Ok(())
            }

            fn encoded_len(&self) -> Option<usize> {
                Some(Self::ENCODED_SIZE)
            }
        }

        impl Decode for $elem {
            fn decode(bytes: &mut Cursor<&[u8]>) -> Result<Self, CodecError> {
                let mut value = [0u8; $elem::ENCODED_SIZE];
                bytes.read_exact(&mut value)?;
                $elem::try_from_bytes(&value, u128::MAX).map_err(|e| {
                    CodecError::Other(Box::new(e) as Box<dyn std::error::Error + 'static + Send + Sync>)
                })
            }
        }

        impl FieldElement for $elem {
            const ENCODED_SIZE: usize = $encoding_size;
            fn inv(&self) -> Self {
                Self($fp.inv(self.0))
            }

            fn try_from_random(bytes: &[u8]) -> Result<Self, FieldError> {
                $elem::try_from_bytes(bytes, $fp.bit_mask)
            }

            fn zero() -> Self {
                Self(0)
            }

            fn one() -> Self {
                Self($fp.roots[0])
            }
        }

        impl FieldElementWithInteger for $elem {
            type Integer = $int;

            fn pow(&self, exp: Self::Integer) -> Self {
                Self($fp.pow(self.0, u128::try_from(exp).unwrap()))
            }

            fn modulus() -> Self::Integer {
                $fp.p as $int
            }
        }

        impl FftFriendlyFieldElement for $elem {
            fn generator() -> Self {
                Self($fp.g)
            }

            fn generator_order() -> Self::Integer {
                1 << (Self::Integer::try_from($fp.num_roots).unwrap())
            }

            fn root(l: usize) -> Option<Self> {
                if l < min($fp.roots.len(), $fp.num_roots+1) {
                    Some(Self($fp.roots[l]))
                } else {
                    None
                }
            }
        }
    };
}

impl Integer for u32 {
    type TryFromUsizeError = <Self as TryFrom<usize>>::Error;
    type TryIntoU64Error = <Self as TryInto<u64>>::Error;

    fn zero() -> Self {
        0
    }

    fn one() -> Self {
        1
    }
}

impl Integer for u64 {
    type TryFromUsizeError = <Self as TryFrom<usize>>::Error;
    type TryIntoU64Error = <Self as TryInto<u64>>::Error;

    fn zero() -> Self {
        0
    }

    fn one() -> Self {
        1
    }
}

impl Integer for u128 {
    type TryFromUsizeError = <Self as TryFrom<usize>>::Error;
    type TryIntoU64Error = <Self as TryInto<u64>>::Error;

    fn zero() -> Self {
        0
    }

    fn one() -> Self {
        1
    }
}

make_field!(
    /// Same as Field32, but encoded in little endian for compatibility with Prio v2.
    FieldPrio2,
    u32,
    FP32,
    4,
);

make_field!(
    /// `GF(18446744069414584321)`, a 64-bit field.
    Field64,
    u64,
    FP64,
    8,
);

make_field!(
    /// `GF(340282366920938462946865773367900766209)`, a 128-bit field.
    Field128,
    u128,
    FP128,
    16,
);

/// Merge two vectors of fields by summing other_vector into accumulator.
///
/// # Errors
///
/// Fails if the two vectors do not have the same length.
pub(crate) fn merge_vector<F: FieldElement>(
    accumulator: &mut [F],
    other_vector: &[F],
) -> Result<(), FieldError> {
    if accumulator.len() != other_vector.len() {
        return Err(FieldError::InputSizeMismatch);
    }
    for (a, o) in accumulator.iter_mut().zip(other_vector.iter()) {
        *a += *o;
    }

    Ok(())
}

/// Outputs an additive secret sharing of the input.

/// Generate a vector of uniformly distributed random field elements.
pub fn random_vector<F: FieldElement>(len: usize) -> Result<Vec<F>, PrngError> {
    Ok(Prng::new()?.take(len).collect())
}

/// `encode_fieldvec` serializes a type that is equivalent to a vector of field elements.
#[inline(always)]
pub(crate) fn encode_fieldvec<F: FieldElement, T: AsRef<[F]>>(
    val: T,
    bytes: &mut Vec<u8>,
) -> Result<(), CodecError> {
    for elem in val.as_ref() {
        elem.encode(bytes)?;
    }
    Ok(())
}

/// `decode_fieldvec` deserializes some number of field elements from a cursor, and advances the
/// cursor's position.
pub(crate) fn decode_fieldvec<F: FieldElement>(
    count: usize,
    input: &mut Cursor<&[u8]>,
) -> Result<Vec<F>, CodecError> {
    let mut vec = Vec::with_capacity(count);
    let mut buffer = [0u8; 64];
    assert!(
        buffer.len() >= F::ENCODED_SIZE,
        "field is too big for buffer"
    );
    for _ in 0..count {
        input.read_exact(&mut buffer[..F::ENCODED_SIZE])?;
        vec.push(
            F::try_from(&buffer[..F::ENCODED_SIZE]).map_err(|e| CodecError::Other(Box::new(e)))?,
        );
    }
    Ok(vec)
}
