// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use crate::error::Error;

pub struct ConcatSlice<'first, 'second> {
    slices: (&'first [u8], &'second [u8]),
    ptr: usize,
}

impl<'first, 'second> ConcatSlice<'first, 'second> {
    pub fn new(slice0: &'first [u8], slice1: &'second [u8]) -> Self {
        Self {
            slices: (slice0, slice1),
            ptr: 0,
        }
    }

    #[allow(clippy::len_without_is_empty)]
    pub fn len(&self) -> usize {
        self.slices.0.len() + self.slices.1.len()
    }

    pub fn remaining_slices(&self) -> (&'first [u8], &'second [u8]) {
        let (slice0, slice1) = self.slices;
        let total_len = self.len();
        let ptr = self.ptr;
        if ptr >= total_len {
            (&[], &[])
        } else if let Some(second_slice_ptr) = ptr.checked_sub(slice0.len()) {
            (&[], &slice1[second_slice_ptr..])
        } else {
            (&slice0[ptr..], slice1)
        }
    }

    pub fn advance(&mut self, bytes: usize) {
        self.ptr += bytes;
    }

    pub fn peek<'out>(&self, out_buf: &'out mut [u8]) -> &'out mut [u8] {
        let (slice0, slice1) = self.remaining_slices();
        let total_len = slice0.len() + slice1.len();

        let out_bytes = out_buf.len().min(total_len);
        let out_buf = &mut out_buf[..out_bytes];

        if out_bytes <= slice0.len() {
            out_buf.copy_from_slice(&slice0[..out_bytes]);
        } else {
            let (out_first, out_second) = out_buf.split_at_mut(slice0.len());
            out_first.copy_from_slice(slice0);
            out_second.copy_from_slice(&slice1[..out_second.len()]);
        }

        out_buf
    }

    pub fn fill_vec(&mut self, max_bytes: Option<usize>, v: &mut Vec<u8>) -> Result<usize, Error> {
        let (slice0, slice1) = self.remaining_slices();
        let total_len = slice0.len() + slice1.len();

        let out_bytes = max_bytes.unwrap_or(usize::MAX).min(total_len);
        v.try_reserve(out_bytes)?;

        if out_bytes <= slice0.len() {
            v.extend_from_slice(&slice0[..out_bytes]);
        } else {
            let second_slice_len = out_bytes - slice0.len();
            v.extend_from_slice(slice0);
            v.extend_from_slice(&slice1[..second_slice_len]);
        }

        self.advance(out_bytes);
        Ok(out_bytes)
    }
}
