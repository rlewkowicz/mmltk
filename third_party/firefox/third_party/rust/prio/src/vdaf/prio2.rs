// SPDX-License-Identifier: MPL-2.0

//! Backwards-compatible port of the ENPA Prio system to a VDAF.

use crate::{
    codec::{CodecError, Decode, Encode, ParameterizedDecode},
    field::{
        decode_fieldvec, FftFriendlyFieldElement, FieldElement, FieldElementWithInteger, FieldPrio2,
    },
    prng::Prng,
    vdaf::{
        prio2::{
            client::{self as v2_client, proof_length},
            server as v2_server,
        },
        xof::Seed,
        Aggregatable, AggregateShare, Aggregator, Client, Collector, OutputShare,
        PrepareTransition, Share, ShareDecodingParameter, Vdaf, VdafError,
    },
};
use hmac::{Hmac, Mac};
use rand_core::RngCore;
use sha2::Sha256;
use std::{convert::TryFrom, io::Cursor};
use subtle::{Choice, ConstantTimeEq};

mod client;
mod server;

/// The Prio2 VDAF. It supports the same measurement type as
/// [`Prio3SumVec`](crate::vdaf::prio3::Prio3SumVec) with `bits == 1` but uses the proof system and
/// finite field deployed in ENPA.
#[derive(Clone, Debug)]
pub struct Prio2 {
    input_len: usize,
}

impl Prio2 {
    /// Returns an instance of the VDAF for the given input length.
    pub fn new(input_len: usize) -> Result<Self, VdafError> {
        let n = (input_len + 1).next_power_of_two();
        if let Ok(size) = u32::try_from(2 * n) {
            if size > FieldPrio2::generator_order() {
                return Err(VdafError::Uncategorized(
                    "input size exceeds field capacity".into(),
                ));
            }
        } else {
            return Err(VdafError::Uncategorized(
                "input size exceeds memory capacity".into(),
            ));
        }

        Ok(Prio2 { input_len })
    }

    /// Prepare an input share for aggregation using the given field element `query_rand` to
    /// compute the verifier share.
    ///
    /// In the [`Aggregator`] trait implementation for [`Prio2`], the query randomness is computed
    /// jointly by the Aggregators. This method is designed to be used in applications, like ENPA,
    /// in which the query randomness is instead chosen by a third-party.
    pub fn prepare_init_with_query_rand(
        &self,
        query_rand: FieldPrio2,
        input_share: &Share<FieldPrio2, 32>,
        is_leader: bool,
    ) -> Result<(Prio2PrepareState, Prio2PrepareShare), VdafError> {
        let expanded_data: Option<Vec<FieldPrio2>> = match input_share {
            Share::Leader(_) => None,
            Share::Helper(ref seed) => {
                let prng = Prng::from_prio2_seed(seed.as_ref());
                Some(prng.take(proof_length(self.input_len)).collect())
            }
        };
        let data = match input_share {
            Share::Leader(ref data) => data,
            Share::Helper(_) => expanded_data.as_ref().unwrap(),
        };

        let verifier_share = v2_server::generate_verification_message(
            self.input_len,
            query_rand,
            data, 
            is_leader,
        )
        .map_err(|e| VdafError::Uncategorized(e.to_string()))?;

        let truncated_share = match input_share {
            Share::Leader(data) => Share::Leader(data[..self.input_len].to_vec()),
            Share::Helper(seed) => Share::Helper(seed.clone()),
        };

        Ok((
            Prio2PrepareState(truncated_share),
            Prio2PrepareShare(verifier_share),
        ))
    }

    /// Choose a random point for polynomial evaluation.
    ///
    /// The point returned is not one of the roots used for polynomial interpolation.
    pub(crate) fn choose_eval_at<S>(&self, prng: &mut Prng<FieldPrio2, S>) -> FieldPrio2
    where
        S: RngCore,
    {
        let n = (self.input_len + 1).next_power_of_two();
        let proof_length = 2 * n;
        loop {
            let eval_at: FieldPrio2 = prng.get();
            if eval_at.pow(u32::try_from(proof_length).unwrap()) != FieldPrio2::one() {
                return eval_at;
            }
        }
    }
}

impl Vdaf for Prio2 {
    type Measurement = Vec<u32>;
    type AggregateResult = Vec<u32>;
    type AggregationParam = ();
    type PublicShare = ();
    type InputShare = Share<FieldPrio2, 32>;
    type OutputShare = OutputShare<FieldPrio2>;
    type AggregateShare = AggregateShare<FieldPrio2>;

    fn algorithm_id(&self) -> u32 {
        0xFFFF0000
    }

    fn num_aggregators(&self) -> usize {
        2
    }
}

impl Client<16> for Prio2 {
    fn shard(
        &self,
        measurement: &Vec<u32>,
        _nonce: &[u8; 16],
    ) -> Result<(Self::PublicShare, Vec<Share<FieldPrio2, 32>>), VdafError> {
        if measurement.len() != self.input_len {
            return Err(VdafError::Uncategorized("incorrect input length".into()));
        }
        let mut input: Vec<FieldPrio2> = Vec::with_capacity(measurement.len());
        for int in measurement {
            input.push((*int).into());
        }

        let mut mem = v2_client::ClientMemory::new(self.input_len)?;
        let copy_data = |share_data: &mut [FieldPrio2]| {
            share_data[..].clone_from_slice(&input);
        };
        let mut leader_data = mem.prove_with(self.input_len, copy_data);

        let helper_seed = Seed::generate()?;
        let helper_prng = Prng::from_prio2_seed(helper_seed.as_ref());
        for (s1, d) in leader_data.iter_mut().zip(helper_prng.into_iter()) {
            *s1 -= d;
        }

        Ok((
            (),
            vec![Share::Leader(leader_data), Share::Helper(helper_seed)],
        ))
    }
}

/// State of each [`Aggregator`] during the Preparation phase.
#[derive(Clone, Debug)]
pub struct Prio2PrepareState(Share<FieldPrio2, 32>);

impl PartialEq for Prio2PrepareState {
    fn eq(&self, other: &Self) -> bool {
        self.ct_eq(other).into()
    }
}

impl Eq for Prio2PrepareState {}

impl ConstantTimeEq for Prio2PrepareState {
    fn ct_eq(&self, other: &Self) -> Choice {
        self.0.ct_eq(&other.0)
    }
}

impl Encode for Prio2PrepareState {
    fn encode(&self, bytes: &mut Vec<u8>) -> Result<(), CodecError> {
        self.0.encode(bytes)
    }

    fn encoded_len(&self) -> Option<usize> {
        self.0.encoded_len()
    }
}

impl<'a> ParameterizedDecode<(&'a Prio2, usize)> for Prio2PrepareState {
    fn decode_with_param(
        (prio2, agg_id): &(&'a Prio2, usize),
        bytes: &mut Cursor<&[u8]>,
    ) -> Result<Self, CodecError> {
        let share_decoder = if *agg_id == 0 {
            ShareDecodingParameter::Leader(prio2.input_len)
        } else {
            ShareDecodingParameter::Helper
        };
        let out_share = Share::decode_with_param(&share_decoder, bytes)?;
        Ok(Self(out_share))
    }
}

/// Message emitted by each [`Aggregator`] during the Preparation phase.
#[derive(Clone, Debug)]
pub struct Prio2PrepareShare(v2_server::VerificationMessage<FieldPrio2>);

impl Encode for Prio2PrepareShare {
    fn encode(&self, bytes: &mut Vec<u8>) -> Result<(), CodecError> {
        self.0.f_r.encode(bytes)?;
        self.0.g_r.encode(bytes)?;
        self.0.h_r.encode(bytes)
    }

    fn encoded_len(&self) -> Option<usize> {
        Some(FieldPrio2::ENCODED_SIZE * 3)
    }
}

impl ParameterizedDecode<Prio2PrepareState> for Prio2PrepareShare {
    fn decode_with_param(
        _state: &Prio2PrepareState,
        bytes: &mut Cursor<&[u8]>,
    ) -> Result<Self, CodecError> {
        Ok(Self(v2_server::VerificationMessage {
            f_r: FieldPrio2::decode(bytes)?,
            g_r: FieldPrio2::decode(bytes)?,
            h_r: FieldPrio2::decode(bytes)?,
        }))
    }
}

impl Aggregator<32, 16> for Prio2 {
    type PrepareState = Prio2PrepareState;
    type PrepareShare = Prio2PrepareShare;
    type PrepareMessage = ();

    fn prepare_init(
        &self,
        agg_key: &[u8; 32],
        agg_id: usize,
        _agg_param: &Self::AggregationParam,
        nonce: &[u8; 16],
        _public_share: &Self::PublicShare,
        input_share: &Share<FieldPrio2, 32>,
    ) -> Result<(Prio2PrepareState, Prio2PrepareShare), VdafError> {
        let is_leader = role_try_from(agg_id)?;

        // In the ENPA Prio system, the query randomness is generated by a third party and
        let mut mac = Hmac::<Sha256>::new_from_slice(agg_key).unwrap();
        mac.update(nonce);
        let hmac_tag = mac.finalize();
        let mut prng = Prng::from_prio2_seed(&hmac_tag.into_bytes().into());
        let query_rand = self.choose_eval_at(&mut prng);

        self.prepare_init_with_query_rand(query_rand, input_share, is_leader)
    }

    fn prepare_shares_to_prepare_message<M: IntoIterator<Item = Prio2PrepareShare>>(
        &self,
        _: &Self::AggregationParam,
        inputs: M,
    ) -> Result<(), VdafError> {
        let verifier_shares: Vec<v2_server::VerificationMessage<FieldPrio2>> =
            inputs.into_iter().map(|msg| msg.0).collect();
        if verifier_shares.len() != 2 {
            return Err(VdafError::Uncategorized(
                "wrong number of verifier shares".into(),
            ));
        }

        if !v2_server::is_valid_share(&verifier_shares[0], &verifier_shares[1]) {
            return Err(VdafError::Uncategorized(
                "proof verifier check failed".into(),
            ));
        }

        Ok(())
    }

    fn prepare_next(
        &self,
        state: Prio2PrepareState,
        _input: (),
    ) -> Result<PrepareTransition<Self, 32, 16>, VdafError> {
        let data = match state.0 {
            Share::Leader(data) => data,
            Share::Helper(seed) => {
                let prng = Prng::from_prio2_seed(seed.as_ref());
                prng.take(self.input_len).collect()
            }
        };
        Ok(PrepareTransition::Finish(OutputShare::from(data)))
    }

    fn aggregate<M: IntoIterator<Item = OutputShare<FieldPrio2>>>(
        &self,
        _agg_param: &Self::AggregationParam,
        out_shares: M,
    ) -> Result<AggregateShare<FieldPrio2>, VdafError> {
        let mut agg_share = AggregateShare(vec![FieldPrio2::zero(); self.input_len]);
        for out_share in out_shares.into_iter() {
            agg_share.accumulate(&out_share)?;
        }

        Ok(agg_share)
    }
}

impl Collector for Prio2 {
    fn unshard<M: IntoIterator<Item = AggregateShare<FieldPrio2>>>(
        &self,
        _agg_param: &Self::AggregationParam,
        agg_shares: M,
        _num_measurements: usize,
    ) -> Result<Vec<u32>, VdafError> {
        let mut agg = AggregateShare(vec![FieldPrio2::zero(); self.input_len]);
        for agg_share in agg_shares.into_iter() {
            agg.merge(&agg_share)?;
        }

        Ok(agg.0.into_iter().map(u32::from).collect())
    }
}

impl<'a> ParameterizedDecode<(&'a Prio2, usize)> for Share<FieldPrio2, 32> {
    fn decode_with_param(
        (prio2, agg_id): &(&'a Prio2, usize),
        bytes: &mut Cursor<&[u8]>,
    ) -> Result<Self, CodecError> {
        let is_leader = role_try_from(*agg_id).map_err(|e| CodecError::Other(Box::new(e)))?;
        let decoder = if is_leader {
            ShareDecodingParameter::Leader(proof_length(prio2.input_len))
        } else {
            ShareDecodingParameter::Helper
        };

        Share::decode_with_param(&decoder, bytes)
    }
}

impl<'a, F> ParameterizedDecode<(&'a Prio2, &'a ())> for OutputShare<F>
where
    F: FieldElement,
{
    fn decode_with_param(
        (prio2, _): &(&'a Prio2, &'a ()),
        bytes: &mut Cursor<&[u8]>,
    ) -> Result<Self, CodecError> {
        decode_fieldvec(prio2.input_len, bytes).map(Self)
    }
}

impl<'a, F> ParameterizedDecode<(&'a Prio2, &'a ())> for AggregateShare<F>
where
    F: FieldElement,
{
    fn decode_with_param(
        (prio2, _): &(&'a Prio2, &'a ()),
        bytes: &mut Cursor<&[u8]>,
    ) -> Result<Self, CodecError> {
        decode_fieldvec(prio2.input_len, bytes).map(Self)
    }
}

fn role_try_from(agg_id: usize) -> Result<bool, VdafError> {
    match agg_id {
        0 => Ok(true),
        1 => Ok(false),
        _ => Err(VdafError::Uncategorized("unexpected aggregator id".into())),
    }
}
