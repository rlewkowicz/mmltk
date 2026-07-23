// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use neqo_common::{Bytes, Encoder, qdebug};

use super::{hframe::HFrameType, reader::FrameDecoder};
use crate::Res;

pub const CAPSULE_TYPE_DATAGRAM: HFrameType = HFrameType(0x00);

#[derive(PartialEq, Eq, Debug, Clone)]
pub enum Capsule {
    Datagram { payload: Bytes },
}

impl Capsule {
    pub const fn capsule_type(&self) -> u64 {
        match self {
            Self::Datagram { .. } => CAPSULE_TYPE_DATAGRAM.0,
        }
    }

    pub fn encode(&self, enc: &mut Encoder) {
        enc.encode_varint(self.capsule_type());
        match self {
            Self::Datagram { payload } => {
                enc.encode_vvec(payload.as_ref());
            }
        }
    }
}

impl FrameDecoder<Self> for Capsule {
    fn decode(frame_type: HFrameType, _frame_len: u64, data: Option<&[u8]>) -> Res<Option<Self>> {
        if frame_type == CAPSULE_TYPE_DATAGRAM
            && let Some(payload) = data
        {
            qdebug!("Decoded Datagram Capsule len={}", payload.len());
            return Ok(Some(Self::Datagram {
                payload: Bytes::from(payload.to_vec()),
            }));
        }
        Ok(None)
    }

    fn is_known_type(frame_type: HFrameType) -> bool {
        frame_type == CAPSULE_TYPE_DATAGRAM
    }
}
