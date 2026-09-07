// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use super::hframe::HFrameType;
use crate::{Res, frames::reader::FrameDecoder};

#[derive(PartialEq, Eq, Debug)]
pub enum Frame {
}

impl FrameDecoder<Self> for Frame {
    fn decode(_frame_type: HFrameType, _frame_len: u64, _data: Option<&[u8]>) -> Res<Option<Self>> {
        Ok(None)
    }

    fn is_known_type(_frame_type: HFrameType) -> bool {
        false
    }
}
