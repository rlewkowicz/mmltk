// SPDX-License-Identifier: MPL-2.0

//! Verifiable Distributed Aggregation Functions (VDAFs) as described in
//! [[draft-irtf-cfrg-vdaf-08]].
//!
//! [draft-irtf-cfrg-vdaf-08]: https://datatracker.ietf.org/doc/draft-irtf-cfrg-vdaf/08/

#[cfg(feature = "experimental")]
use crate::dp::DifferentialPrivacyStrategy;
#[cfg(all(feature = "crypto-dependencies", feature = "experimental"))]
use crate::idpf::IdpfError;
#[cfg(all(feature = "crypto-dependencies", feature = "experimental"))]
use crate::vidpf::VidpfError;
use crate::{
    codec::{CodecError, Decode, Encode, ParameterizedDecode},
    field::{encode_fieldvec, merge_vector, FieldElement, FieldError},
    flp::FlpError,
    prng::PrngError,
    vdaf::xof::Seed,
};
use serde::{Deserialize, Serialize};
use std::{error::Error, fmt::Debug, io::Cursor};
use subtle::{Choice, ConstantTimeEq};

/// A component of the domain-separation tag, used to bind the VDAF operations to the document
/// version. This will be revised with each draft with breaking changes.
pub(crate) const VERSION: u8 = 8;

/// Errors emitted by this module.
#[derive(Debug, thiserror::Error)]
#[non_exhaustive]
pub enum VdafError {
    /// An error occurred.
    #[error("vdaf error: {0}")]
    Uncategorized(String),

    /// Field error.
    #[error("field error: {0}")]
    Field(#[from] FieldError),

    /// An error occured while parsing a message.
    #[error("io error: {0}")]
    IoError(#[from] std::io::Error),

    /// FLP error.
    #[error("flp error: {0}")]
    Flp(#[from] FlpError),

    /// PRNG error.
    #[error("prng error: {0}")]
    Prng(#[from] PrngError),

    /// Failure when calling getrandom().
    #[error("getrandom: {0}")]
    GetRandom(#[from] getrandom::Error),

    /// IDPF error.
    #[cfg(all(feature = "crypto-dependencies", feature = "experimental"))]
    #[error("idpf error: {0}")]
    Idpf(#[from] IdpfError),

    /// VIDPF error.
    #[cfg(all(feature = "crypto-dependencies", feature = "experimental"))]
    #[error("vidpf error: {0}")]
    Vidpf(#[from] VidpfError),

    /// Errors from other VDAFs.
    #[error(transparent)]
    Other(Box<dyn Error + 'static + Send + Sync>),
}

/// An additive share of a vector of field elements.
#[derive(Clone, Debug)]
pub enum Share<F, const SEED_SIZE: usize> {
    /// An uncompressed share, typically sent to the leader.
    Leader(Vec<F>),

    /// A compressed share, typically sent to the helper.
    Helper(Seed<SEED_SIZE>),
}

impl<F: ConstantTimeEq, const SEED_SIZE: usize> PartialEq for Share<F, SEED_SIZE> {
    fn eq(&self, other: &Self) -> bool {
        self.ct_eq(other).into()
    }
}

impl<F: ConstantTimeEq, const SEED_SIZE: usize> Eq for Share<F, SEED_SIZE> {}

impl<F: ConstantTimeEq, const SEED_SIZE: usize> ConstantTimeEq for Share<F, SEED_SIZE> {
    fn ct_eq(&self, other: &Self) -> subtle::Choice {
        match (self, other) {
            (Share::Leader(self_val), Share::Leader(other_val)) => self_val.ct_eq(other_val),
            (Share::Helper(self_val), Share::Helper(other_val)) => self_val.ct_eq(other_val),
            _ => Choice::from(0),
        }
    }
}

/// Parameters needed to decode a [`Share`]
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum ShareDecodingParameter<const SEED_SIZE: usize> {
    Leader(usize),
    Helper,
}

impl<F: FieldElement, const SEED_SIZE: usize> ParameterizedDecode<ShareDecodingParameter<SEED_SIZE>>
    for Share<F, SEED_SIZE>
{
    fn decode_with_param(
        decoding_parameter: &ShareDecodingParameter<SEED_SIZE>,
        bytes: &mut Cursor<&[u8]>,
    ) -> Result<Self, CodecError> {
        match decoding_parameter {
            ShareDecodingParameter::Leader(share_length) => {
                let mut data = Vec::with_capacity(*share_length);
                for _ in 0..*share_length {
                    data.push(F::decode(bytes)?)
                }
                Ok(Self::Leader(data))
            }
            ShareDecodingParameter::Helper => {
                let seed = Seed::decode(bytes)?;
                Ok(Self::Helper(seed))
            }
        }
    }
}

impl<F: FieldElement, const SEED_SIZE: usize> Encode for Share<F, SEED_SIZE> {
    fn encode(&self, bytes: &mut Vec<u8>) -> Result<(), CodecError> {
        match self {
            Share::Leader(share_data) => {
                for x in share_data {
                    x.encode(bytes)?;
                }
                Ok(())
            }
            Share::Helper(share_seed) => share_seed.encode(bytes),
        }
    }

    fn encoded_len(&self) -> Option<usize> {
        match self {
            Share::Leader(share_data) => {
                Some(share_data.len() * F::ENCODED_SIZE)
            }
            Share::Helper(share_seed) => share_seed.encoded_len(),
        }
    }
}

/// The base trait for VDAF schemes. This trait is inherited by traits [`Client`], [`Aggregator`],
/// and [`Collector`], which define the roles of the various parties involved in the execution of
/// the VDAF.
pub trait Vdaf: Clone + Debug {
    /// The type of Client measurement to be aggregated.
    type Measurement: Clone + Debug;

    /// The aggregate result of the VDAF execution.
    type AggregateResult: Clone + Debug;

    /// The aggregation parameter, used by the Aggregators to map their input shares to output
    /// shares.
    type AggregationParam: Clone + Debug + Decode + Encode;

    /// A public share sent by a Client.
    type PublicShare: Clone + Debug + ParameterizedDecode<Self> + Encode;

    /// An input share sent by a Client.
    type InputShare: Clone + Debug + for<'a> ParameterizedDecode<(&'a Self, usize)> + Encode;

    /// An output share recovered from an input share by an Aggregator.
    type OutputShare: Clone
        + Debug
        + for<'a> ParameterizedDecode<(&'a Self, &'a Self::AggregationParam)>
        + Encode;

    /// An Aggregator's share of the aggregate result.
    type AggregateShare: Aggregatable<OutputShare = Self::OutputShare>
        + for<'a> ParameterizedDecode<(&'a Self, &'a Self::AggregationParam)>
        + Encode;

    /// Return the VDAF's algorithm ID.
    fn algorithm_id(&self) -> u32;

    /// The number of Aggregators. The Client generates as many input shares as there are
    /// Aggregators.
    fn num_aggregators(&self) -> usize;

    /// Generate the domain separation tag for this VDAF. The output is used for domain separation
    /// by the XOF.
    fn domain_separation_tag(&self, usage: u16) -> [u8; 8] {
        let mut dst = [0_u8; 8];
        dst[0] = VERSION;
        dst[1] = 0; 
        dst[2..6].copy_from_slice(&(self.algorithm_id()).to_be_bytes());
        dst[6..8].copy_from_slice(&usage.to_be_bytes());
        dst
    }
}

/// The Client's role in the execution of a VDAF.
pub trait Client<const NONCE_SIZE: usize>: Vdaf {
    /// Shards a measurement into a public share and a sequence of input shares, one for each
    /// Aggregator.
    ///
    /// Implements `Vdaf::shard` from [VDAF].
    ///
    /// [VDAF]: https://datatracker.ietf.org/doc/html/draft-irtf-cfrg-vdaf-08#section-5.1
    fn shard(
        &self,
        measurement: &Self::Measurement,
        nonce: &[u8; NONCE_SIZE],
    ) -> Result<(Self::PublicShare, Vec<Self::InputShare>), VdafError>;
}

/// The Aggregator's role in the execution of a VDAF.
pub trait Aggregator<const VERIFY_KEY_SIZE: usize, const NONCE_SIZE: usize>: Vdaf {
    /// State of the Aggregator during the Prepare process.
    type PrepareState: Clone + Debug + PartialEq + Eq;

    /// The type of messages sent by each aggregator at each round of the Prepare Process.
    ///
    /// Decoding takes a [`Self::PrepareState`] as a parameter; this [`Self::PrepareState`] may be
    /// associated with any aggregator involved in the execution of the VDAF.
    type PrepareShare: Clone + Debug + ParameterizedDecode<Self::PrepareState> + Encode;

    /// Result of preprocessing a round of preparation shares. This is used by all aggregators as an
    /// input to the next round of the Prepare Process.
    ///
    /// Decoding takes a [`Self::PrepareState`] as a parameter; this [`Self::PrepareState`] may be
    /// associated with any aggregator involved in the execution of the VDAF.
    type PrepareMessage: Clone
        + Debug
        + PartialEq
        + Eq
        + ParameterizedDecode<Self::PrepareState>
        + Encode;

    /// Begins the Prepare process with the other Aggregators. The [`Self::PrepareState`] returned
    /// is passed to [`Self::prepare_next`] to get this aggregator's first-round prepare message.
    ///
    /// Implements `Vdaf.prep_init` from [VDAF].
    ///
    /// [VDAF]: https://datatracker.ietf.org/doc/html/draft-irtf-cfrg-vdaf-08#section-5.2
    fn prepare_init(
        &self,
        verify_key: &[u8; VERIFY_KEY_SIZE],
        agg_id: usize,
        agg_param: &Self::AggregationParam,
        nonce: &[u8; NONCE_SIZE],
        public_share: &Self::PublicShare,
        input_share: &Self::InputShare,
    ) -> Result<(Self::PrepareState, Self::PrepareShare), VdafError>;

    /// Preprocess a round of preparation shares into a single input to [`Self::prepare_next`].
    ///
    /// Implements `Vdaf.prep_shares_to_prep` from [VDAF].
    ///
    /// [VDAF]: https://datatracker.ietf.org/doc/html/draft-irtf-cfrg-vdaf-08#section-5.2
    fn prepare_shares_to_prepare_message<M: IntoIterator<Item = Self::PrepareShare>>(
        &self,
        agg_param: &Self::AggregationParam,
        inputs: M,
    ) -> Result<Self::PrepareMessage, VdafError>;

    /// Compute the next state transition from the current state and the previous round of input
    /// messages. If this returns [`PrepareTransition::Continue`], then the returned
    /// [`Self::PrepareShare`] should be combined with the other Aggregators' `PrepareShare`s from
    /// this round and passed into another call to this method. This continues until this method
    /// returns [`PrepareTransition::Finish`], at which point the returned output share may be
    /// aggregated. If the method returns an error, the aggregator should consider its input share
    /// invalid and not attempt to process it any further.
    ///
    /// Implements `Vdaf.prep_next` from [VDAF].
    ///
    /// [VDAF]: https://datatracker.ietf.org/doc/html/draft-irtf-cfrg-vdaf-08#section-5.2
    fn prepare_next(
        &self,
        state: Self::PrepareState,
        input: Self::PrepareMessage,
    ) -> Result<PrepareTransition<Self, VERIFY_KEY_SIZE, NONCE_SIZE>, VdafError>;

    /// Aggregates a sequence of output shares into an aggregate share.
    fn aggregate<M: IntoIterator<Item = Self::OutputShare>>(
        &self,
        agg_param: &Self::AggregationParam,
        output_shares: M,
    ) -> Result<Self::AggregateShare, VdafError>;
}

/// Aggregator that implements differential privacy with Aggregator-side noise addition.
#[cfg(feature = "experimental")]
#[cfg_attr(docsrs, doc(cfg(feature = "experimental")))]
pub trait AggregatorWithNoise<
    const VERIFY_KEY_SIZE: usize,
    const NONCE_SIZE: usize,
    DPStrategy: DifferentialPrivacyStrategy,
>: Aggregator<VERIFY_KEY_SIZE, NONCE_SIZE>
{
    /// Adds noise to an aggregate share such that the aggregate result is differentially private
    /// as long as one Aggregator is honest.
    fn add_noise_to_agg_share(
        &self,
        dp_strategy: &DPStrategy,
        agg_param: &Self::AggregationParam,
        agg_share: &mut Self::AggregateShare,
        num_measurements: usize,
    ) -> Result<(), VdafError>;
}

/// The Collector's role in the execution of a VDAF.
pub trait Collector: Vdaf {
    /// Combines aggregate shares into the aggregate result.
    fn unshard<M: IntoIterator<Item = Self::AggregateShare>>(
        &self,
        agg_param: &Self::AggregationParam,
        agg_shares: M,
        num_measurements: usize,
    ) -> Result<Self::AggregateResult, VdafError>;
}

/// A state transition of an Aggregator during the Prepare process.
#[derive(Clone, Debug)]
pub enum PrepareTransition<
    V: Aggregator<VERIFY_KEY_SIZE, NONCE_SIZE>,
    const VERIFY_KEY_SIZE: usize,
    const NONCE_SIZE: usize,
> {
    /// Continue processing.
    Continue(V::PrepareState, V::PrepareShare),

    /// Finish processing and return the output share.
    Finish(V::OutputShare),
}

/// An aggregate share resulting from aggregating output shares together that
/// can merged with aggregate shares of the same type.
pub trait Aggregatable: Clone + Debug + From<Self::OutputShare> {
    /// Type of output shares that can be accumulated into an aggregate share.
    type OutputShare;

    /// Update an aggregate share by merging it with another (`agg_share`).
    fn merge(&mut self, agg_share: &Self) -> Result<(), VdafError>;

    /// Update an aggregate share by adding `output_share`.
    fn accumulate(&mut self, output_share: &Self::OutputShare) -> Result<(), VdafError>;
}

/// An output share comprised of a vector of field elements.
#[derive(Clone)]
pub struct OutputShare<F>(Vec<F>);

impl<F: ConstantTimeEq> PartialEq for OutputShare<F> {
    fn eq(&self, other: &Self) -> bool {
        self.ct_eq(other).into()
    }
}

impl<F: ConstantTimeEq> Eq for OutputShare<F> {}

impl<F: ConstantTimeEq> ConstantTimeEq for OutputShare<F> {
    fn ct_eq(&self, other: &Self) -> Choice {
        self.0.ct_eq(&other.0)
    }
}

impl<F> AsRef<[F]> for OutputShare<F> {
    fn as_ref(&self) -> &[F] {
        &self.0
    }
}

impl<F> From<Vec<F>> for OutputShare<F> {
    fn from(other: Vec<F>) -> Self {
        Self(other)
    }
}

impl<F: FieldElement> Encode for OutputShare<F> {
    fn encode(&self, bytes: &mut Vec<u8>) -> Result<(), CodecError> {
        encode_fieldvec(&self.0, bytes)
    }

    fn encoded_len(&self) -> Option<usize> {
        Some(F::ENCODED_SIZE * self.0.len())
    }
}

impl<F> Debug for OutputShare<F> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_tuple("OutputShare").finish()
    }
}

/// An aggregate share comprised of a vector of field elements.
///
/// This is suitable for VDAFs where both output shares and aggregate shares are vectors of field
/// elements, and output shares need no special transformation to be merged into an aggregate share.
#[derive(Clone, Debug, Serialize, Deserialize)]

pub struct AggregateShare<F>(Vec<F>);

impl<F> From<Vec<F>> for AggregateShare<F> {
    fn from(other: Vec<F>) -> Self {
        Self(other)
    }
}

impl<F: ConstantTimeEq> PartialEq for AggregateShare<F> {
    fn eq(&self, other: &Self) -> bool {
        self.ct_eq(other).into()
    }
}

impl<F: ConstantTimeEq> Eq for AggregateShare<F> {}

impl<F: ConstantTimeEq> ConstantTimeEq for AggregateShare<F> {
    fn ct_eq(&self, other: &Self) -> subtle::Choice {
        self.0.ct_eq(&other.0)
    }
}

impl<F: FieldElement> AsRef<[F]> for AggregateShare<F> {
    fn as_ref(&self) -> &[F] {
        &self.0
    }
}

impl<F> From<OutputShare<F>> for AggregateShare<F> {
    fn from(other: OutputShare<F>) -> Self {
        Self(other.0)
    }
}

impl<F: FieldElement> Aggregatable for AggregateShare<F> {
    type OutputShare = OutputShare<F>;

    fn merge(&mut self, agg_share: &Self) -> Result<(), VdafError> {
        self.sum(agg_share.as_ref())
    }

    fn accumulate(&mut self, output_share: &Self::OutputShare) -> Result<(), VdafError> {
        self.sum(output_share.as_ref())
    }
}

impl<F: FieldElement> AggregateShare<F> {
    fn sum(&mut self, other: &[F]) -> Result<(), VdafError> {
        merge_vector(&mut self.0, other).map_err(Into::into)
    }
}

impl<F: FieldElement> Encode for AggregateShare<F> {
    fn encode(&self, bytes: &mut Vec<u8>) -> Result<(), CodecError> {
        encode_fieldvec(&self.0, bytes)
    }

    fn encoded_len(&self) -> Option<usize> {
        Some(F::ENCODED_SIZE * self.0.len())
    }
}

/// Utilities for testing VDAFs.




#[cfg(all(feature = "crypto-dependencies", feature = "experimental"))]
#[cfg_attr(docsrs, doc(cfg(all(feature = "crypto-dependencies", feature = "experimental"))))]
pub mod poplar1;
#[cfg(all(feature = "crypto-dependencies", feature = "experimental"))]
#[cfg_attr(docsrs, doc(cfg(all(feature = "crypto-dependencies", feature = "experimental"))))]
pub mod prio2;
pub mod prio3;
pub mod xof;
