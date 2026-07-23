// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{cmp::min, mem};

use crate::codec::Decoder;

#[derive(Clone, Debug, Default)]
pub struct IncrementalDecoderUint {
    v: u64,
    remaining: Option<usize>,
}

impl IncrementalDecoderUint {
    #[must_use]
    pub fn min_remaining(&self) -> usize {
        self.remaining.unwrap_or(1)
    }

    /// Consume some data.
    ///
    /// # Panics
    ///
    /// Never, but this is not something the compiler can tell.
    pub fn consume(&mut self, dv: &mut Decoder) -> Option<u64> {
        if let Some(r) = &mut self.remaining {
            let amount = min(*r, dv.remaining());
            if amount < 8 {
                self.v <<= amount * 8;
            }
            self.v |= dv.decode_n(amount)?;
            *r -= amount;
            (*r == 0).then_some(self.v)
        } else {
            let (v, remaining) = dv.decode_uint::<u8>().map_or_else(
                || unreachable!(),
                |b| {
                    (
                        u64::from(b & 0x3f),
                        match b >> 6 {
                            0 => 0,
                            1 => 1,
                            2 => 3,
                            3 => 7,
                            _ => unreachable!(),
                        },
                    )
                },
            );
            self.remaining = Some(remaining);
            self.v = v;
            (remaining == 0).then_some(v)
        }
    }

    #[must_use]
    pub const fn decoding_in_progress(&self) -> bool {
        self.remaining.is_some()
    }
}

#[derive(Clone, Debug)]
pub struct IncrementalDecoderBuffer {
    v: Vec<u8>,
    remaining: usize,
}

impl IncrementalDecoderBuffer {
    #[must_use]
    pub const fn new(n: usize) -> Self {
        Self {
            v: Vec::new(),
            remaining: n,
        }
    }

    #[must_use]
    pub const fn min_remaining(&self) -> usize {
        self.remaining
    }

    /// Consume some bytes from the decoder.
    ///
    /// # Panics
    ///
    /// Never; but rust doesn't know that.
    pub fn consume(&mut self, dv: &mut Decoder) -> Option<Vec<u8>> {
        let amount = min(self.remaining, dv.remaining());
        let b = dv.decode(amount)?;
        self.v.extend_from_slice(b);
        self.remaining -= amount;
        (self.remaining == 0).then(|| mem::take(&mut self.v))
    }
}

#[derive(Clone, Debug)]
pub struct IncrementalDecoderIgnore {
    remaining: usize,
}

impl IncrementalDecoderIgnore {
    /// Make a new ignoring decoder.
    ///
    /// # Panics
    ///
    /// If the amount to ignore is zero.
    #[must_use]
    pub fn new(n: usize) -> Self {
        assert_ne!(n, 0);
        Self { remaining: n }
    }

    #[must_use]
    pub const fn min_remaining(&self) -> usize {
        self.remaining
    }

    pub fn consume(&mut self, dv: &mut Decoder) -> bool {
        let amount = min(self.remaining, dv.remaining());
        _ = dv.decode(amount);
        self.remaining -= amount;
        self.remaining == 0
    }
}
