//! This module implements the incremental distributed point function (IDPF) described in
//! [[draft-irtf-cfrg-vdaf-08]].
//!
//! [draft-irtf-cfrg-vdaf-08]: https://datatracker.ietf.org/doc/draft-irtf-cfrg-vdaf/08/

use crate::{
    codec::{CodecError, Decode, Encode, ParameterizedDecode},
    field::{FieldElement, FieldElementExt},
    vdaf::{
        xof::{Seed, XofFixedKeyAes128Key},
        VdafError, VERSION,
    },
};
use bitvec::{
    bitvec,
    boxed::BitBox,
    prelude::{Lsb0, Msb0},
    slice::BitSlice,
    vec::BitVec,
    view::BitView,
};
use rand_core::RngCore;
use std::{
    collections::{HashMap, VecDeque},
    fmt::Debug,
    io::{Cursor, Read},
    iter::zip,
    ops::{Add, AddAssign, ControlFlow, Index, Sub},
};
use subtle::{Choice, ConditionallyNegatable, ConditionallySelectable, ConstantTimeEq};

/// IDPF-related errors.
#[derive(Debug, thiserror::Error)]
#[non_exhaustive]
pub enum IdpfError {
    /// Error from incompatible shares at different levels.
    #[error("tried to merge shares from incompatible levels")]
    MismatchedLevel,

    /// Invalid parameter, indicates an invalid input to either [`Idpf::gen`] or [`Idpf::eval`].
    #[error("invalid parameter: {0}")]
    InvalidParameter(String),
}

/// An index used as the input to an IDPF evaluation.
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct IdpfInput {
    /// The index as a boxed bit slice.
    index: BitBox,
}

impl IdpfInput {
    /// Convert a slice of bytes into an IDPF input, where the bits of each byte are processed in
    /// MSB-to-LSB order. (Subsequent bytes are processed in their natural order.)
    pub fn from_bytes(bytes: &[u8]) -> IdpfInput {
        let bit_slice_u8_storage = bytes.view_bits::<Msb0>();
        let mut bit_vec_usize_storage = bitvec![0; bit_slice_u8_storage.len()];
        bit_vec_usize_storage.clone_from_bitslice(bit_slice_u8_storage);
        IdpfInput {
            index: bit_vec_usize_storage.into_boxed_bitslice(),
        }
    }

    /// Convert a slice of booleans into an IDPF input.
    pub fn from_bools(bools: &[bool]) -> IdpfInput {
        let bits = bools.iter().collect::<BitVec>();
        IdpfInput {
            index: bits.into_boxed_bitslice(),
        }
    }

    /// Create a new IDPF input by appending to this input.
    pub fn clone_with_suffix(&self, suffix: &[bool]) -> IdpfInput {
        let mut vec = BitVec::with_capacity(self.index.len() + suffix.len());
        vec.extend_from_bitslice(&self.index);
        vec.extend(suffix);
        IdpfInput {
            index: vec.into_boxed_bitslice(),
        }
    }

    /// Get the length of the input in bits.
    pub fn len(&self) -> usize {
        self.index.len()
    }

    /// Check if the input is empty, i.e. it does not contain any bits.
    pub fn is_empty(&self) -> bool {
        self.index.is_empty()
    }

    /// Get an iterator over the bits that make up this input.
    pub fn iter(&self) -> impl DoubleEndedIterator<Item = bool> + '_ {
        self.index.iter().by_vals()
    }

    /// Convert the IDPF into a byte slice. If the length of the underlying bit vector is not a
    /// multiple of `8`, then the least significant bits of the last byte are `0`-padded.
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut vec = BitVec::<u8, Msb0>::with_capacity(self.index.len());
        vec.extend_from_bitslice(&self.index);
        vec.set_uninitialized(false);
        vec.into_vec()
    }

    /// Return the `level`-bit prefix of this IDPF input.
    pub fn prefix(&self, level: usize) -> Self {
        Self {
            index: self.index[..=level].to_owned().into(),
        }
    }

    /// Return the bit at the specified level if the level is in bounds.
    pub fn get(&self, level: usize) -> Option<bool> {
        self.index.get(level).as_deref().copied()
    }
}

impl From<BitVec<usize, Lsb0>> for IdpfInput {
    fn from(bit_vec: BitVec<usize, Lsb0>) -> Self {
        IdpfInput {
            index: bit_vec.into_boxed_bitslice(),
        }
    }
}

impl From<BitBox<usize, Lsb0>> for IdpfInput {
    fn from(bit_box: BitBox<usize, Lsb0>) -> Self {
        IdpfInput { index: bit_box }
    }
}

impl<I> Index<I> for IdpfInput
where
    BitSlice: Index<I>,
{
    type Output = <BitSlice as Index<I>>::Output;

    fn index(&self, index: I) -> &Self::Output {
        &self.index[index]
    }
}

/// Trait for values to be programmed into an IDPF.
///
/// Values must form an Abelian group, so that they can be secret-shared, and the group operation
/// must be represented by [`Add`]. Values must be encodable and decodable, without need for a
/// decoding parameter. Values can be pseudorandomly generated, with a uniform probability
/// distribution, from XOF output.
pub trait IdpfValue:
    Add<Output = Self>
    + AddAssign
    + Sub<Output = Self>
    + ConditionallyNegatable
    + Encode
    + ParameterizedDecode<Self::ValueParameter>
    + Sized
{
    /// Any run-time parameters needed to produce a value.
    type ValueParameter;

    /// Generate a pseudorandom value from a seed stream.
    fn generate<S>(seed_stream: &mut S, parameter: &Self::ValueParameter) -> Self
    where
        S: RngCore;

    /// Returns the additive identity.
    fn zero(parameter: &Self::ValueParameter) -> Self;

    /// Conditionally select between two values. Implementations must perform this operation in
    /// constant time.
    ///
    /// This is the same as in [`subtle::ConditionallySelectable`], but without the [`Copy`] bound.
    fn conditional_select(a: &Self, b: &Self, choice: Choice) -> Self;
}

impl<F> IdpfValue for F
where
    F: FieldElement,
{
    type ValueParameter = ();

    fn generate<S>(seed_stream: &mut S, _: &()) -> Self
    where
        S: RngCore,
    {
        let mut buffer = [0u8; 64];
        assert!(
            buffer.len() >= F::ENCODED_SIZE,
            "field is too big for buffer"
        );
        loop {
            seed_stream.fill_bytes(&mut buffer[..F::ENCODED_SIZE]);
            match F::from_random_rejection(&buffer[..F::ENCODED_SIZE]) {
                ControlFlow::Break(x) => return x,
                ControlFlow::Continue(()) => continue,
            }
        }
    }

    fn zero(_: &()) -> Self {
        <Self as FieldElement>::zero()
    }

    fn conditional_select(a: &Self, b: &Self, choice: Choice) -> Self {
        <F as ConditionallySelectable>::conditional_select(a, b, choice)
    }
}

/// An output from evaluation of an IDPF at some level and index.
#[derive(Debug, PartialEq, Eq)]
pub enum IdpfOutputShare<VI, VL> {
    /// An IDPF output share corresponding to an inner tree node.
    Inner(VI),
    /// An IDPF output share corresponding to a leaf tree node.
    Leaf(VL),
}

impl<VI, VL> IdpfOutputShare<VI, VL>
where
    VI: IdpfValue,
    VL: IdpfValue,
{
    /// Combine two output share values into one.
    pub fn merge(self, other: Self) -> Result<IdpfOutputShare<VI, VL>, IdpfError> {
        match (self, other) {
            (IdpfOutputShare::Inner(mut self_value), IdpfOutputShare::Inner(other_value)) => {
                self_value += other_value;
                Ok(IdpfOutputShare::Inner(self_value))
            }
            (IdpfOutputShare::Leaf(mut self_value), IdpfOutputShare::Leaf(other_value)) => {
                self_value += other_value;
                Ok(IdpfOutputShare::Leaf(self_value))
            }
            (_, _) => Err(IdpfError::MismatchedLevel),
        }
    }
}

fn extend(seed: &[u8; 16], xof_fixed_key: &XofFixedKeyAes128Key) -> ([[u8; 16]; 2], [Choice; 2]) {
    let mut seed_stream = xof_fixed_key.with_seed(seed);

    let mut seeds = [[0u8; 16], [0u8; 16]];
    seed_stream.fill_bytes(&mut seeds[0]);
    seed_stream.fill_bytes(&mut seeds[1]);

    let control_bits_0 = seeds[0].as_ref()[0] & 1;
    let control_bits_1 = seeds[1].as_ref()[0] & 1;
    seeds[0].as_mut()[0] &= 0xfe;
    seeds[1].as_mut()[0] &= 0xfe;

    (seeds, [control_bits_0.into(), control_bits_1.into()])
}

fn convert<V>(
    seed: &[u8; 16],
    xof_fixed_key: &XofFixedKeyAes128Key,
    parameter: &V::ValueParameter,
) -> ([u8; 16], V)
where
    V: IdpfValue,
{
    let mut seed_stream = xof_fixed_key.with_seed(seed);

    let mut next_seed = [0u8; 16];
    seed_stream.fill_bytes(&mut next_seed);

    (next_seed, V::generate(&mut seed_stream, parameter))
}

/// Helper method to update seeds, update control bits, and output the correction word for one level
/// of the IDPF key generation process.
fn generate_correction_word<V>(
    input_bit: Choice,
    value: V,
    parameter: &V::ValueParameter,
    keys: &mut [[u8; 16]; 2],
    control_bits: &mut [Choice; 2],
    extend_xof_fixed_key: &XofFixedKeyAes128Key,
    convert_xof_fixed_key: &XofFixedKeyAes128Key,
) -> IdpfCorrectionWord<V>
where
    V: IdpfValue,
{
    let (seed_0, control_bits_0) = extend(&keys[0], extend_xof_fixed_key);
    let (seed_1, control_bits_1) = extend(&keys[1], extend_xof_fixed_key);

    let (keep, lose) = (input_bit, !input_bit);

    let cw_seed = xor_seeds(
        &conditional_select_seed(lose, &seed_0),
        &conditional_select_seed(lose, &seed_1),
    );
    let cw_control_bits = [
        control_bits_0[0] ^ control_bits_1[0] ^ input_bit ^ Choice::from(1),
        control_bits_0[1] ^ control_bits_1[1] ^ input_bit,
    ];
    let cw_control_bits_keep =
        Choice::conditional_select(&cw_control_bits[0], &cw_control_bits[1], keep);

    let previous_control_bits = *control_bits;
    let control_bits_0_keep =
        Choice::conditional_select(&control_bits_0[0], &control_bits_0[1], keep);
    let control_bits_1_keep =
        Choice::conditional_select(&control_bits_1[0], &control_bits_1[1], keep);
    control_bits[0] = control_bits_0_keep ^ (cw_control_bits_keep & previous_control_bits[0]);
    control_bits[1] = control_bits_1_keep ^ (cw_control_bits_keep & previous_control_bits[1]);

    let seed_0_keep = conditional_select_seed(keep, &seed_0);
    let seed_1_keep = conditional_select_seed(keep, &seed_1);
    let seeds_corrected = [
        conditional_xor_seeds(&seed_0_keep, &cw_seed, previous_control_bits[0]),
        conditional_xor_seeds(&seed_1_keep, &cw_seed, previous_control_bits[1]),
    ];

    let (new_key_0, elements_0) =
        convert::<V>(&seeds_corrected[0], convert_xof_fixed_key, parameter);
    let (new_key_1, elements_1) =
        convert::<V>(&seeds_corrected[1], convert_xof_fixed_key, parameter);

    keys[0] = new_key_0;
    keys[1] = new_key_1;

    let mut cw_value = value - elements_0 + elements_1;
    cw_value.conditional_negate(control_bits[1]);

    IdpfCorrectionWord {
        seed: cw_seed,
        control_bits: cw_control_bits,
        value: cw_value,
    }
}

/// Helper function to evaluate one level of an IDPF. This updates the seed and control bit
/// arguments that are passed in.
#[allow(clippy::too_many_arguments)]
fn eval_next<V>(
    is_leader: bool,
    parameter: &V::ValueParameter,
    key: &mut [u8; 16],
    control_bit: &mut Choice,
    correction_word: &IdpfCorrectionWord<V>,
    input_bit: Choice,
    extend_xof_fixed_key: &XofFixedKeyAes128Key,
    convert_xof_fixed_key: &XofFixedKeyAes128Key,
) -> V
where
    V: IdpfValue,
{
    let (mut seeds, mut control_bits) = extend(key, extend_xof_fixed_key);

    seeds[0] = conditional_xor_seeds(&seeds[0], &correction_word.seed, *control_bit);
    control_bits[0] ^= correction_word.control_bits[0] & *control_bit;
    seeds[1] = conditional_xor_seeds(&seeds[1], &correction_word.seed, *control_bit);
    control_bits[1] ^= correction_word.control_bits[1] & *control_bit;

    let seed_corrected = conditional_select_seed(input_bit, &seeds);
    *control_bit = Choice::conditional_select(&control_bits[0], &control_bits[1], input_bit);

    let (new_key, elements) = convert::<V>(&seed_corrected, convert_xof_fixed_key, parameter);
    *key = new_key;

    let mut out =
        elements + V::conditional_select(&V::zero(parameter), &correction_word.value, *control_bit);
    out.conditional_negate(Choice::from((!is_leader) as u8));
    out
}

/// This defines a family of IDPFs (incremental distributed point functions) with certain types of
/// values at inner tree nodes and at leaf tree nodes.
///
/// IDPF keys can be generated by providing an input and programmed outputs for each tree level to
/// [`Idpf::gen`].
pub struct Idpf<VI, VL>
where
    VI: IdpfValue,
    VL: IdpfValue,
{
    inner_node_value_parameter: VI::ValueParameter,
    leaf_node_value_parameter: VL::ValueParameter,
}

impl<VI, VL> Idpf<VI, VL>
where
    VI: IdpfValue,
    VL: IdpfValue,
{
    /// Construct an [`Idpf`] instance with the given run-time parameters needed for inner and leaf
    /// values.
    pub fn new(
        inner_node_value_parameter: VI::ValueParameter,
        leaf_node_value_parameter: VL::ValueParameter,
    ) -> Self {
        Self {
            inner_node_value_parameter,
            leaf_node_value_parameter,
        }
    }

    pub(crate) fn gen_with_random<M: IntoIterator<Item = VI>>(
        &self,
        input: &IdpfInput,
        inner_values: M,
        leaf_value: VL,
        binder: &[u8],
        random: &[[u8; 16]; 2],
    ) -> Result<(IdpfPublicShare<VI, VL>, [Seed<16>; 2]), VdafError> {
        let bits = input.len();

        let initial_keys: [Seed<16>; 2] =
            [Seed::from_bytes(random[0]), Seed::from_bytes(random[1])];

        let extend_dst = [
            VERSION, 1, 
            0, 0, 0, 0, 
            0, 0, 
        ];
        let convert_dst = [
            VERSION, 1, 
            0, 0, 0, 0, 
            0, 1, 
        ];
        let extend_xof_fixed_key = XofFixedKeyAes128Key::new(&extend_dst, binder);
        let convert_xof_fixed_key = XofFixedKeyAes128Key::new(&convert_dst, binder);

        let mut keys = [initial_keys[0].0, initial_keys[1].0];
        let mut control_bits = [Choice::from(0u8), Choice::from(1u8)];
        let mut inner_correction_words = Vec::with_capacity(bits - 1);

        for (level, value) in inner_values.into_iter().enumerate() {
            if level >= bits - 1 {
                return Err(IdpfError::InvalidParameter(
                    "too many values were supplied".to_string(),
                )
                .into());
            }
            inner_correction_words.push(generate_correction_word::<VI>(
                Choice::from(input[level] as u8),
                value,
                &self.inner_node_value_parameter,
                &mut keys,
                &mut control_bits,
                &extend_xof_fixed_key,
                &convert_xof_fixed_key,
            ));
        }
        if inner_correction_words.len() != bits - 1 {
            return Err(
                IdpfError::InvalidParameter("too few values were supplied".to_string()).into(),
            );
        }
        let leaf_correction_word = generate_correction_word::<VL>(
            Choice::from(input[bits - 1] as u8),
            leaf_value,
            &self.leaf_node_value_parameter,
            &mut keys,
            &mut control_bits,
            &extend_xof_fixed_key,
            &convert_xof_fixed_key,
        );
        let public_share = IdpfPublicShare {
            inner_correction_words,
            leaf_correction_word,
        };

        Ok((public_share, initial_keys))
    }

    /// The IDPF key generation algorithm.
    ///
    /// Generate and return a sequence of IDPF shares for `input`. The parameters `inner_values`
    /// and `leaf_value` provide the output values for each successive level of the prefix tree.
    pub fn gen<M>(
        &self,
        input: &IdpfInput,
        inner_values: M,
        leaf_value: VL,
        binder: &[u8],
    ) -> Result<(IdpfPublicShare<VI, VL>, [Seed<16>; 2]), VdafError>
    where
        M: IntoIterator<Item = VI>,
    {
        if input.is_empty() {
            return Err(
                IdpfError::InvalidParameter("invalid number of bits: 0".to_string()).into(),
            );
        }
        let mut random = [[0u8; 16]; 2];
        for random_seed in random.iter_mut() {
            getrandom::getrandom(random_seed)?;
        }
        self.gen_with_random(input, inner_values, leaf_value, binder, &random)
    }

    /// Evaluate an IDPF share on `prefix`, starting from a particular tree level with known
    /// intermediate values.
    #[allow(clippy::too_many_arguments)]
    fn eval_from_node(
        &self,
        is_leader: bool,
        public_share: &IdpfPublicShare<VI, VL>,
        start_level: usize,
        mut key: [u8; 16],
        mut control_bit: Choice,
        prefix: &IdpfInput,
        binder: &[u8],
        cache: &mut dyn IdpfCache,
    ) -> Result<IdpfOutputShare<VI, VL>, IdpfError> {
        let bits = public_share.inner_correction_words.len() + 1;

        let extend_dst = [
            VERSION, 1, 
            0, 0, 0, 0, 
            0, 0, 
        ];
        let convert_dst = [
            VERSION, 1, 
            0, 0, 0, 0, 
            0, 1, 
        ];
        let extend_xof_fixed_key = XofFixedKeyAes128Key::new(&extend_dst, binder);
        let convert_xof_fixed_key = XofFixedKeyAes128Key::new(&convert_dst, binder);

        let mut last_inner_output = None;
        for ((correction_word, input_bit), level) in public_share.inner_correction_words
            [start_level..]
            .iter()
            .zip(prefix[start_level..].iter())
            .zip(start_level..)
        {
            last_inner_output = Some(eval_next(
                is_leader,
                &self.inner_node_value_parameter,
                &mut key,
                &mut control_bit,
                correction_word,
                Choice::from(*input_bit as u8),
                &extend_xof_fixed_key,
                &convert_xof_fixed_key,
            ));
            let cache_key = &prefix[..=level];
            cache.insert(cache_key, &(key, control_bit.unwrap_u8()));
        }

        if prefix.len() == bits {
            let leaf_output = eval_next(
                is_leader,
                &self.leaf_node_value_parameter,
                &mut key,
                &mut control_bit,
                &public_share.leaf_correction_word,
                Choice::from(prefix[bits - 1] as u8),
                &extend_xof_fixed_key,
                &convert_xof_fixed_key,
            );
            Ok(IdpfOutputShare::Leaf(leaf_output))
        } else {
            Ok(IdpfOutputShare::Inner(last_inner_output.unwrap()))
        }
    }

    /// The IDPF key evaluation algorithm.
    ///
    /// Evaluate an IDPF share on `prefix`.
    pub fn eval(
        &self,
        agg_id: usize,
        public_share: &IdpfPublicShare<VI, VL>,
        key: &Seed<16>,
        prefix: &IdpfInput,
        binder: &[u8],
        cache: &mut dyn IdpfCache,
    ) -> Result<IdpfOutputShare<VI, VL>, IdpfError> {
        let bits = public_share.inner_correction_words.len() + 1;
        if agg_id > 1 {
            return Err(IdpfError::InvalidParameter(format!(
                "invalid aggregator ID {agg_id}"
            )));
        }
        let is_leader = agg_id == 0;
        if prefix.is_empty() {
            return Err(IdpfError::InvalidParameter("empty prefix".to_string()));
        }
        if prefix.len() > bits {
            return Err(IdpfError::InvalidParameter(format!(
                "prefix length ({}) exceeds configured number of bits ({})",
                prefix.len(),
                bits,
            )));
        }

        if prefix.len() > 1 {
            let mut cache_key = &prefix[..prefix.len() - 1];
            while !cache_key.is_empty() {
                if let Some((key, control_bit)) = cache.get(cache_key) {
                    return self.eval_from_node(
                        is_leader,
                        public_share,
                         cache_key.len(),
                        key,
                        Choice::from(control_bit),
                        prefix,
                        binder,
                        cache,
                    );
                }
                cache_key = &cache_key[..cache_key.len() - 1];
            }
        }
        self.eval_from_node(
            is_leader,
            public_share,
             0,
            key.0,
             Choice::from((!is_leader) as u8),
            prefix,
            binder,
            cache,
        )
    }
}

/// An IDPF public share. This contains the list of correction words used by all parties when
/// evaluating the IDPF.
#[derive(Debug, Clone)]
pub struct IdpfPublicShare<VI, VL> {
    /// Correction words for each inner node level.
    inner_correction_words: Vec<IdpfCorrectionWord<VI>>,
    /// Correction word for the leaf node level.
    leaf_correction_word: IdpfCorrectionWord<VL>,
}

impl<VI, VL> ConstantTimeEq for IdpfPublicShare<VI, VL>
where
    VI: ConstantTimeEq,
    VL: ConstantTimeEq,
{
    fn ct_eq(&self, other: &Self) -> Choice {
        self.inner_correction_words
            .ct_eq(&other.inner_correction_words)
            & self.leaf_correction_word.ct_eq(&other.leaf_correction_word)
    }
}

impl<VI, VL> PartialEq for IdpfPublicShare<VI, VL>
where
    VI: ConstantTimeEq,
    VL: ConstantTimeEq,
{
    fn eq(&self, other: &Self) -> bool {
        self.ct_eq(other).into()
    }
}

impl<VI, VL> Eq for IdpfPublicShare<VI, VL>
where
    VI: ConstantTimeEq,
    VL: ConstantTimeEq,
{
}

impl<VI, VL> Encode for IdpfPublicShare<VI, VL>
where
    VI: Encode,
    VL: Encode,
{
    fn encode(&self, bytes: &mut Vec<u8>) -> Result<(), CodecError> {
        let mut control_bits: BitVec<u8, Lsb0> =
            BitVec::with_capacity(self.inner_correction_words.len() * 2 + 2);
        for correction_words in self.inner_correction_words.iter() {
            control_bits.extend(correction_words.control_bits.iter().map(|x| bool::from(*x)));
        }
        control_bits.extend(
            self.leaf_correction_word
                .control_bits
                .iter()
                .map(|x| bool::from(*x)),
        );
        control_bits.set_uninitialized(false);
        let mut packed_control = control_bits.into_vec();
        bytes.append(&mut packed_control);

        for correction_words in self.inner_correction_words.iter() {
            Seed(correction_words.seed).encode(bytes)?;
            correction_words.value.encode(bytes)?;
        }
        Seed(self.leaf_correction_word.seed).encode(bytes)?;
        self.leaf_correction_word.value.encode(bytes)
    }

    fn encoded_len(&self) -> Option<usize> {
        let control_bits_count = (self.inner_correction_words.len() + 1) * 2;
        let mut len = (control_bits_count + 7) / 8 + (self.inner_correction_words.len() + 1) * 16;
        for correction_words in self.inner_correction_words.iter() {
            len += correction_words.value.encoded_len()?;
        }
        len += self.leaf_correction_word.value.encoded_len()?;
        Some(len)
    }
}

impl<VI, VL> ParameterizedDecode<usize> for IdpfPublicShare<VI, VL>
where
    VI: Decode,
    VL: Decode,
{
    fn decode_with_param(bits: &usize, bytes: &mut Cursor<&[u8]>) -> Result<Self, CodecError> {
        let packed_control_len = (bits + 3) / 4;
        let mut packed = vec![0u8; packed_control_len];
        bytes.read_exact(&mut packed)?;
        let unpacked_control_bits: BitVec<u8, Lsb0> = BitVec::from_vec(packed);

        let mut inner_correction_words = Vec::with_capacity(bits - 1);
        for chunk in unpacked_control_bits[0..(bits - 1) * 2].chunks(2) {
            let control_bits = [(chunk[0] as u8).into(), (chunk[1] as u8).into()];
            let seed = Seed::decode(bytes)?.0;
            let value = VI::decode(bytes)?;
            inner_correction_words.push(IdpfCorrectionWord {
                seed,
                control_bits,
                value,
            })
        }

        let control_bits = [
            (unpacked_control_bits[(bits - 1) * 2] as u8).into(),
            (unpacked_control_bits[bits * 2 - 1] as u8).into(),
        ];
        let seed = Seed::decode(bytes)?.0;
        let value = VL::decode(bytes)?;
        let leaf_correction_word = IdpfCorrectionWord {
            seed,
            control_bits,
            value,
        };

        if unpacked_control_bits[bits * 2..].any() {
            return Err(CodecError::UnexpectedValue);
        }

        Ok(IdpfPublicShare {
            inner_correction_words,
            leaf_correction_word,
        })
    }
}

#[derive(Debug, Clone)]
struct IdpfCorrectionWord<V> {
    seed: [u8; 16],
    control_bits: [Choice; 2],
    value: V,
}

impl<V> ConstantTimeEq for IdpfCorrectionWord<V>
where
    V: ConstantTimeEq,
{
    fn ct_eq(&self, other: &Self) -> Choice {
        self.seed.ct_eq(&other.seed)
            & self.control_bits.ct_eq(&other.control_bits)
            & self.value.ct_eq(&other.value)
    }
}

impl<V> PartialEq for IdpfCorrectionWord<V>
where
    V: ConstantTimeEq,
{
    fn eq(&self, other: &Self) -> bool {
        self.ct_eq(other).into()
    }
}

impl<V> Eq for IdpfCorrectionWord<V> where V: ConstantTimeEq {}

pub(crate) fn xor_seeds(left: &[u8; 16], right: &[u8; 16]) -> [u8; 16] {
    let mut seed = [0u8; 16];
    for (a, (b, c)) in left.iter().zip(right.iter().zip(seed.iter_mut())) {
        *c = a ^ b;
    }
    seed
}

fn and_seeds(left: &[u8; 16], right: &[u8; 16]) -> [u8; 16] {
    let mut seed = [0u8; 16];
    for (a, (b, c)) in left.iter().zip(right.iter().zip(seed.iter_mut())) {
        *c = a & b;
    }
    seed
}

fn or_seeds(left: &[u8; 16], right: &[u8; 16]) -> [u8; 16] {
    let mut seed = [0u8; 16];
    for (a, (b, c)) in left.iter().zip(right.iter().zip(seed.iter_mut())) {
        *c = a | b;
    }
    seed
}

/// Take a control bit, and fan it out into a byte array that can be used as a mask for XOF seeds,
/// without branching. If the control bit input is 0, all bytes will be equal to 0, and if the
/// control bit input is 1, all bytes will be equal to 255.
fn control_bit_to_seed_mask(control: Choice) -> [u8; 16] {
    let mask = -(control.unwrap_u8() as i8) as u8;
    [mask; 16]
}

/// Take two seeds and a control bit, and return the first seed if the control bit is zero, or the
/// XOR of the two seeds if the control bit is one. This does not branch on the control bit.
pub(crate) fn conditional_xor_seeds(
    normal_input: &[u8; 16],
    switched_input: &[u8; 16],
    control: Choice,
) -> [u8; 16] {
    xor_seeds(
        normal_input,
        &and_seeds(switched_input, &control_bit_to_seed_mask(control)),
    )
}

/// Returns one of two seeds, depending on the value of a selector bit. Does not branch on the
/// selector input or make selector-dependent memory accesses.
pub(crate) fn conditional_select_seed(select: Choice, seeds: &[[u8; 16]; 2]) -> [u8; 16] {
    or_seeds(
        &and_seeds(&control_bit_to_seed_mask(!select), &seeds[0]),
        &and_seeds(&control_bit_to_seed_mask(select), &seeds[1]),
    )
}

/// Interchange the contents of seeds if the choice is 1, otherwise seeds remain unchanged.
pub(crate) fn conditional_swap_seed(lhs: &mut [u8; 16], rhs: &mut [u8; 16], choice: Choice) {
    zip(lhs, rhs).for_each(|(a, b)| u8::conditional_swap(a, b, choice));
}

/// An interface that provides memoization of IDPF computations.
///
/// Each instance of a type implementing `IdpfCache` should only be used with one IDPF key and
/// public share.
///
/// In typical use, IDPFs will be evaluated repeatedly on inputs of increasing length, as part of a
/// protocol executed by multiple participants. Each IDPF evaluation computes keys and control
/// bits corresponding to tree nodes along a path determined by the input to the IDPF. Thus, the
/// values from nodes further up in the tree may be cached and reused in evaluations of subsequent
/// longer inputs. If one IDPF input is a prefix of another input, then the first input's path down
/// the tree is a prefix of the other input's path.
pub trait IdpfCache {
    /// Fetch cached values for the node identified by the IDPF input.
    fn get(&self, input: &BitSlice) -> Option<([u8; 16], u8)>;

    /// Store values corresponding to the node identified by the IDPF input.
    fn insert(&mut self, input: &BitSlice, values: &([u8; 16], u8));
}

/// A no-op [`IdpfCache`] implementation that always reports a cache miss.
#[derive(Default)]
pub struct NoCache {}

impl NoCache {
    /// Construct a `NoCache` object.
    pub fn new() -> NoCache {
        NoCache::default()
    }
}

impl IdpfCache for NoCache {
    fn get(&self, _: &BitSlice) -> Option<([u8; 16], u8)> {
        None
    }

    fn insert(&mut self, _: &BitSlice, _: &([u8; 16], u8)) {}
}

/// A simple [`IdpfCache`] implementation that caches intermediate results in an in-memory hash map,
/// with no eviction.
#[derive(Default)]
pub struct HashMapCache {
    map: HashMap<BitBox, ([u8; 16], u8)>,
}

impl HashMapCache {
    /// Create a new unpopulated `HashMapCache`.
    pub fn new() -> HashMapCache {
        HashMapCache::default()
    }

    /// Create a new unpopulated `HashMapCache`, with a set pre-allocated capacity.
    pub fn with_capacity(capacity: usize) -> HashMapCache {
        Self {
            map: HashMap::with_capacity(capacity),
        }
    }
}

impl IdpfCache for HashMapCache {
    fn get(&self, input: &BitSlice) -> Option<([u8; 16], u8)> {
        self.map.get(input).cloned()
    }

    fn insert(&mut self, input: &BitSlice, values: &([u8; 16], u8)) {
        if !self.map.contains_key(input) {
            self.map
                .insert(input.to_owned().into_boxed_bitslice(), *values);
        }
    }
}

/// A simple [`IdpfCache`] implementation that caches intermediate results in memory, with
/// first-in-first-out eviction, and lookups via linear probing.
pub struct RingBufferCache {
    ring: VecDeque<(BitBox, [u8; 16], u8)>,
}

impl RingBufferCache {
    /// Create a new unpopulated `RingBufferCache`.
    pub fn new(capacity: usize) -> RingBufferCache {
        Self {
            ring: VecDeque::with_capacity(std::cmp::max(capacity, 1)),
        }
    }
}

impl IdpfCache for RingBufferCache {
    fn get(&self, input: &BitSlice) -> Option<([u8; 16], u8)> {
        for entry in self.ring.iter().rev() {
            if input == entry.0 {
                return Some((entry.1, entry.2));
            }
        }
        None
    }

    fn insert(&mut self, input: &BitSlice, values: &([u8; 16], u8)) {
        if self.ring.len() == self.ring.capacity() {
            self.ring.pop_front();
        }
        self.ring
            .push_back((input.to_owned().into_boxed_bitslice(), values.0, values.1));
    }
}
