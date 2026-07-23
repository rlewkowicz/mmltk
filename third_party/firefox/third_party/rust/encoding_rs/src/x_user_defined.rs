// Copyright Mozilla Foundation. See the COPYRIGHT
// file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use super::*;
use crate::handles::*;
use crate::variant::*;

cfg_if! {
    if #[cfg(feature = "simd-accel")] {
        use simd_funcs::*;
        use core::simd::u16x8;
        use core::simd::cmp::SimdPartialOrd;
        #[rustversion::since(1.95)]
        use core::simd::Select;

        #[inline(always)]
        fn shift_upper(unpacked: u16x8) -> u16x8 {
            let highest_ascii = u16x8::splat(0x7F);
            unpacked + unpacked.simd_gt(highest_ascii).select(u16x8::splat(0xF700), u16x8::splat(0))        }
    } else {
    }
}

pub struct UserDefinedDecoder;

impl UserDefinedDecoder {
    pub fn new() -> VariantDecoder {
        VariantDecoder::UserDefined(UserDefinedDecoder)
    }

    pub fn max_utf16_buffer_length(&self, byte_length: usize) -> Option<usize> {
        Some(byte_length)
    }

    pub fn max_utf8_buffer_length_without_replacement(&self, byte_length: usize) -> Option<usize> {
        byte_length.checked_mul(3)
    }

    pub fn max_utf8_buffer_length(&self, byte_length: usize) -> Option<usize> {
        byte_length.checked_mul(3)
    }

    decoder_function!(
        {},
        {},
        {},
        {
            if b < 0x80 {
                destination_handle.write_ascii(b);
                continue;
            }
            destination_handle.write_upper_bmp(u16::from(b) + 0xF700);
            continue;
        },
        self,
        src_consumed,
        dest,
        source,
        b,
        destination_handle,
        _unread_handle,
        check_space_bmp,
        decode_to_utf8_raw,
        u8,
        Utf8Destination
    );

    #[cfg(not(feature = "simd-accel"))]
    pub fn decode_to_utf16_raw(
        &mut self,
        src: &[u8],
        dst: &mut [u16],
        _last: bool,
    ) -> (DecoderResult, usize, usize) {
        let (pending, length) = if dst.len() < src.len() {
            (DecoderResult::OutputFull, dst.len())
        } else {
            (DecoderResult::InputEmpty, src.len())
        };
        let src_trim = &src[..length];
        let dst_trim = &mut dst[..length];
        src_trim
            .iter()
            .zip(dst_trim.iter_mut())
            .for_each(|(from, to)| {
                *to = {
                    let unit = *from;
                    if unit < 0x80 {
                        u16::from(unit)
                    } else {
                        u16::from(unit) + 0xF700
                    }
                }
            });
        (pending, length, length)
    }

    #[cfg(feature = "simd-accel")]
    pub fn decode_to_utf16_raw(
        &mut self,
        src: &[u8],
        dst: &mut [u16],
        _last: bool,
    ) -> (DecoderResult, usize, usize) {
        let (pending, length) = if dst.len() < src.len() {
            (DecoderResult::OutputFull, dst.len())
        } else {
            (DecoderResult::InputEmpty, src.len())
        };
        let tail_start = length & !0xF;
        let simd_iterations = length >> 4;
        let src_ptr = src.as_ptr();
        let dst_ptr = dst.as_mut_ptr();
        for i in 0..simd_iterations {
            let input = unsafe { load16_unaligned(src_ptr.add(i * 16)) };
            let (first, second) = simd_unpack(input);
            unsafe {
                store8_unaligned(dst_ptr.add(i * 16), shift_upper(first));
                store8_unaligned(dst_ptr.add((i * 16) + 8), shift_upper(second));
            }
        }
        let src_tail = &src[tail_start..length];
        let dst_tail = &mut dst[tail_start..length];
        src_tail
            .iter()
            .zip(dst_tail.iter_mut())
            .for_each(|(from, to)| {
                *to = {
                    let unit = *from;
                    if unit < 0x80 {
                        u16::from(unit)
                    } else {
                        u16::from(unit) + 0xF700
                    }
                }
            });
        (pending, length, length)
    }
}

pub struct UserDefinedEncoder;

impl UserDefinedEncoder {
    pub fn new(encoding: &'static Encoding) -> Encoder {
        Encoder::new(encoding, VariantEncoder::UserDefined(UserDefinedEncoder))
    }

    pub fn max_buffer_length_from_utf16_without_replacement(
        &self,
        u16_length: usize,
    ) -> Option<usize> {
        Some(u16_length)
    }

    pub fn max_buffer_length_from_utf8_without_replacement(
        &self,
        byte_length: usize,
    ) -> Option<usize> {
        Some(byte_length)
    }

    encoder_functions!(
        {},
        {
            if c <= '\u{7F}' {
                destination_handle.write_one(c as u8);
                continue;
            }
            if c < '\u{F780}' || c > '\u{F7FF}' {
                return (
                    EncoderResult::Unmappable(c),
                    unread_handle.consumed(),
                    destination_handle.written(),
                );
            }
            destination_handle.write_one((u32::from(c) - 0xF700) as u8);
            continue;
        },
        self,
        src_consumed,
        source,
        dest,
        c,
        destination_handle,
        unread_handle,
        check_space_one
    );
}

// Any copyright to the test code below this comment is dedicated to the
