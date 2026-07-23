// Copyright Mozilla Foundation. See the COPYRIGHT
// file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use super::*;
use crate::ascii::ascii_to_basic_latin;
use crate::ascii::basic_latin_to_ascii;
use crate::ascii::validate_ascii;
use crate::handles::*;
use crate::mem::convert_utf16_to_utf8_partial;
use crate::variant::*;

cfg_if! {
    if #[cfg(feature = "simd-accel")] {
        use ::core::intrinsics::unlikely;
        use ::core::intrinsics::likely;
    } else {
        #[inline(always)]
        fn unlikely(b: bool) -> bool {
            b
        }
        #[inline(always)]
        fn likely(b: bool) -> bool {
            b
        }
    }
}

#[repr(align(64))] 
pub struct Utf8Data {
    pub table: [u8; 384],
}

// BEGIN GENERATED CODE. PLEASE DO NOT EDIT.

pub static UTF8_DATA: Utf8Data = Utf8Data {
    table: [
        252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 84, 84, 84, 84, 84, 84, 84, 84, 84, 84, 84, 84, 84, 84, 84, 84, 148, 148, 148,
        148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 164, 164, 164, 164, 164,
        164, 164, 164, 164, 164, 164, 164, 164, 164, 164, 164, 164, 164, 164, 164, 164, 164, 164,
        164, 164, 164, 164, 164, 164, 164, 164, 164, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
        252, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 16, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 32, 8, 8, 64, 8, 8, 8, 128, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    ],
};


pub fn utf8_valid_up_to(src: &[u8]) -> usize {
    let mut read = 0;
    'outer: loop {
        let mut byte = {
            let src_remaining = &src[read..];
            match validate_ascii(src_remaining) {
                None => {
                    return src.len();
                }
                Some((non_ascii, consumed)) => {
                    read += consumed;
                    non_ascii
                }
            }
        };
        if likely(read + 4 <= src.len()) {
            'inner: loop {
                if likely(in_inclusive_range8(byte, 0xC2, 0xDF)) {
                    let second = unsafe { *(src.get_unchecked(read + 1)) };
                    if !in_inclusive_range8(second, 0x80, 0xBF) {
                        break 'outer;
                    }
                    read += 2;

                    if likely(read + 4 <= src.len()) {
                        byte = unsafe { *(src.get_unchecked(read)) };
                        if byte < 0x80 {
                            read += 1;
                            continue 'outer;
                        }
                        continue 'inner;
                    }
                    break 'inner;
                }
                if likely(byte < 0xF0) {
                    'three: loop {
                        let second = unsafe { *(src.get_unchecked(read + 1)) };
                        let third = unsafe { *(src.get_unchecked(read + 2)) };
                        if ((UTF8_DATA.table[usize::from(second)]
                            & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) })
                            | (third >> 6))
                            != 2
                        {
                            break 'outer;
                        }
                        read += 3;

                        if likely(read + 4 <= src.len()) {
                            byte = unsafe { *(src.get_unchecked(read)) };
                            if in_inclusive_range8(byte, 0xE0, 0xEF) {
                                continue 'three;
                            }
                            if likely(byte < 0x80) {
                                read += 1;
                                continue 'outer;
                            }
                            continue 'inner;
                        }
                        break 'inner;
                    }
                }
                let second = unsafe { *(src.get_unchecked(read + 1)) };
                let third = unsafe { *(src.get_unchecked(read + 2)) };
                let fourth = unsafe { *(src.get_unchecked(read + 3)) };
                if (u16::from(
                    UTF8_DATA.table[usize::from(second)]
                        & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) },
                ) | u16::from(third >> 6)
                    | (u16::from(fourth & 0xC0) << 2))
                    != 0x202
                {
                    break 'outer;
                }
                read += 4;

                if likely(read + 4 <= src.len()) {
                    byte = unsafe { *(src.get_unchecked(read)) };
                    if byte < 0x80 {
                        read += 1;
                        continue 'outer;
                    }
                    continue 'inner;
                }
                break 'inner;
            }
        }
        'tail: loop {
            if read >= src.len() {
                break 'outer;
            }
            byte = src[read];
            if byte < 0x80 {
                read += 1;
                continue 'tail;
            }
            if in_inclusive_range8(byte, 0xC2, 0xDF) {
                let new_read = read + 2;
                if new_read > src.len() {
                    break 'outer;
                }
                let second = src[read + 1];
                if !in_inclusive_range8(second, 0x80, 0xBF) {
                    break 'outer;
                }
                read += 2;
                continue 'tail;
            }
            if byte < 0xF0 {
                let new_read = read + 3;
                if new_read > src.len() {
                    break 'outer;
                }
                let second = src[read + 1];
                let third = src[read + 2];
                if ((UTF8_DATA.table[usize::from(second)]
                    & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) })
                    | (third >> 6))
                    != 2
                {
                    break 'outer;
                }
                read += 3;
                break 'outer;
            }
            break 'outer;
        }
    }
    read
}

#[cfg_attr(feature = "cargo-clippy", allow(never_loop, cyclomatic_complexity))]
pub fn convert_utf8_to_utf16_up_to_invalid(src: &[u8], dst: &mut [u16]) -> (usize, usize) {
    let mut read = 0;
    let mut written = 0;
    'outer: loop {
        let mut byte = {
            let src_remaining = &src[read..];
            let dst_remaining = &mut dst[written..];
            let length = ::core::cmp::min(src_remaining.len(), dst_remaining.len());
            match unsafe {
                ascii_to_basic_latin(src_remaining.as_ptr(), dst_remaining.as_mut_ptr(), length)
            } {
                None => {
                    read += length;
                    written += length;
                    break 'outer;
                }
                Some((non_ascii, consumed)) => {
                    read += consumed;
                    written += consumed;
                    non_ascii
                }
            }
        };
        if likely(read + 4 <= src.len()) {
            'inner: loop {
                if likely(in_inclusive_range8(byte, 0xC2, 0xDF)) {
                    let second = unsafe { *(src.get_unchecked(read + 1)) };
                    if !in_inclusive_range8(second, 0x80, 0xBF) {
                        break 'outer;
                    }
                    unsafe {
                        *(dst.get_unchecked_mut(written)) =
                            ((u16::from(byte) & 0x1F) << 6) | (u16::from(second) & 0x3F)
                    };
                    read += 2;
                    written += 1;

                    if written == dst.len() {
                        break 'outer;
                    }
                    if likely(read + 4 <= src.len()) {
                        byte = unsafe { *(src.get_unchecked(read)) };
                        if byte < 0x80 {
                            unsafe { *(dst.get_unchecked_mut(written)) = u16::from(byte) };
                            read += 1;
                            written += 1;
                            continue 'outer;
                        }
                        continue 'inner;
                    }
                    break 'inner;
                }
                if likely(byte < 0xF0) {
                    'three: loop {
                        let second = unsafe { *(src.get_unchecked(read + 1)) };
                        let third = unsafe { *(src.get_unchecked(read + 2)) };
                        if ((UTF8_DATA.table[usize::from(second)]
                            & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) })
                            | (third >> 6))
                            != 2
                        {
                            break 'outer;
                        }
                        let point = ((u16::from(byte) & 0xF) << 12)
                            | ((u16::from(second) & 0x3F) << 6)
                            | (u16::from(third) & 0x3F);
                        unsafe { *(dst.get_unchecked_mut(written)) = point };
                        read += 3;
                        written += 1;

                        if written == dst.len() {
                            break 'outer;
                        }
                        if likely(read + 4 <= src.len()) {
                            byte = unsafe { *(src.get_unchecked(read)) };
                            if in_inclusive_range8(byte, 0xE0, 0xEF) {
                                continue 'three;
                            }
                            if likely(byte < 0x80) {
                                unsafe { *(dst.get_unchecked_mut(written)) = u16::from(byte) };
                                read += 1;
                                written += 1;
                                continue 'outer;
                            }
                            continue 'inner;
                        }
                        break 'inner;
                    }
                }
                if written + 1 == dst.len() {
                    break 'outer;
                }
                let second = unsafe { *(src.get_unchecked(read + 1)) };
                let third = unsafe { *(src.get_unchecked(read + 2)) };
                let fourth = unsafe { *(src.get_unchecked(read + 3)) };
                if (u16::from(
                    UTF8_DATA.table[usize::from(second)]
                        & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) },
                ) | u16::from(third >> 6)
                    | (u16::from(fourth & 0xC0) << 2))
                    != 0x202
                {
                    break 'outer;
                }
                let point = ((u32::from(byte) & 0x7) << 18)
                    | ((u32::from(second) & 0x3F) << 12)
                    | ((u32::from(third) & 0x3F) << 6)
                    | (u32::from(fourth) & 0x3F);
                unsafe { *(dst.get_unchecked_mut(written)) = (0xD7C0 + (point >> 10)) as u16 };
                unsafe {
                    *(dst.get_unchecked_mut(written + 1)) = (0xDC00 + (point & 0x3FF)) as u16
                };
                read += 4;
                written += 2;

                if written == dst.len() {
                    break 'outer;
                }
                if likely(read + 4 <= src.len()) {
                    byte = unsafe { *(src.get_unchecked(read)) };
                    if byte < 0x80 {
                        unsafe { *(dst.get_unchecked_mut(written)) = u16::from(byte) };
                        read += 1;
                        written += 1;
                        continue 'outer;
                    }
                    continue 'inner;
                }
                break 'inner;
            }
        }
        'tail: loop {
            if read >= src.len() || written >= dst.len() {
                break 'outer;
            }
            byte = src[read];
            if byte < 0x80 {
                dst[written] = u16::from(byte);
                read += 1;
                written += 1;
                continue 'tail;
            }
            if in_inclusive_range8(byte, 0xC2, 0xDF) {
                let new_read = read + 2;
                if new_read > src.len() {
                    break 'outer;
                }
                let second = src[read + 1];
                if !in_inclusive_range8(second, 0x80, 0xBF) {
                    break 'outer;
                }
                dst[written] = ((u16::from(byte) & 0x1F) << 6) | (u16::from(second) & 0x3F);
                read += 2;
                written += 1;
                continue 'tail;
            }
            if byte < 0xF0 {
                let new_read = read + 3;
                if new_read > src.len() {
                    break 'outer;
                }
                let second = src[read + 1];
                let third = src[read + 2];
                if ((UTF8_DATA.table[usize::from(second)]
                    & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) })
                    | (third >> 6))
                    != 2
                {
                    break 'outer;
                }
                let point = ((u16::from(byte) & 0xF) << 12)
                    | ((u16::from(second) & 0x3F) << 6)
                    | (u16::from(third) & 0x3F);
                dst[written] = point;
                read += 3;
                written += 1;
                break 'outer;
            }
            break 'outer;
        }
    }
    (read, written)
}

pub struct Utf8Decoder {
    code_point: u32,
    bytes_seen: usize,   
    bytes_needed: usize, 
    lower_boundary: u8,
    upper_boundary: u8,
}

impl Utf8Decoder {
    pub fn new_inner() -> Utf8Decoder {
        Utf8Decoder {
            code_point: 0,
            bytes_seen: 0,
            bytes_needed: 0,
            lower_boundary: 0x80u8,
            upper_boundary: 0xBFu8,
        }
    }

    pub fn new() -> VariantDecoder {
        VariantDecoder::Utf8(Utf8Decoder::new_inner())
    }

    pub fn in_neutral_state(&self) -> bool {
        self.bytes_needed == 0
    }

    fn extra_from_state(&self) -> usize {
        if self.bytes_needed == 0 {
            0
        } else {
            self.bytes_seen + 1
        }
    }

    pub fn max_utf16_buffer_length(&self, byte_length: usize) -> Option<usize> {
        byte_length.checked_add(1 + self.extra_from_state())
    }

    pub fn max_utf8_buffer_length_without_replacement(&self, byte_length: usize) -> Option<usize> {
        byte_length.checked_add(3 + self.extra_from_state())
    }

    pub fn max_utf8_buffer_length(&self, byte_length: usize) -> Option<usize> {
        checked_add(
            3,
            checked_mul(3, byte_length.checked_add(self.extra_from_state())),
        )
    }

    decoder_functions!(
        {},
        {
            if self.bytes_needed == 0 {
                dest.copy_utf8_up_to_invalid_from(&mut source);
            }
        },
        {
            if self.bytes_needed != 0 {
                let bad_bytes = (self.bytes_seen + 1) as u8;
                self.code_point = 0;
                self.bytes_needed = 0;
                self.bytes_seen = 0;
                return (
                    DecoderResult::Malformed(bad_bytes, 0),
                    src_consumed,
                    dest.written(),
                );
            }
        },
        {
            if self.bytes_needed == 0 {
                if b < 0x80u8 {
                    destination_handle.write_ascii(b);
                    continue;
                }
                if b < 0xC2u8 {
                    return (
                        DecoderResult::Malformed(1, 0),
                        unread_handle.consumed(),
                        destination_handle.written(),
                    );
                }
                if b < 0xE0u8 {
                    self.bytes_needed = 1;
                    self.code_point = u32::from(b) & 0x1F;
                    continue;
                }
                if b < 0xF0u8 {
                    if b == 0xE0u8 {
                        self.lower_boundary = 0xA0u8;
                    } else if b == 0xEDu8 {
                        self.upper_boundary = 0x9Fu8;
                    }
                    self.bytes_needed = 2;
                    self.code_point = u32::from(b) & 0xF;
                    continue;
                }
                if b < 0xF5u8 {
                    if b == 0xF0u8 {
                        self.lower_boundary = 0x90u8;
                    } else if b == 0xF4u8 {
                        self.upper_boundary = 0x8Fu8;
                    }
                    self.bytes_needed = 3;
                    self.code_point = u32::from(b) & 0x7;
                    continue;
                }
                return (
                    DecoderResult::Malformed(1, 0),
                    unread_handle.consumed(),
                    destination_handle.written(),
                );
            }
            if !(b >= self.lower_boundary && b <= self.upper_boundary) {
                let bad_bytes = (self.bytes_seen + 1) as u8;
                self.code_point = 0;
                self.bytes_needed = 0;
                self.bytes_seen = 0;
                self.lower_boundary = 0x80u8;
                self.upper_boundary = 0xBFu8;
                return (
                    DecoderResult::Malformed(bad_bytes, 0),
                    unread_handle.unread(),
                    destination_handle.written(),
                );
            }
            self.lower_boundary = 0x80u8;
            self.upper_boundary = 0xBFu8;
            self.code_point = (self.code_point << 6) | (u32::from(b) & 0x3F);
            self.bytes_seen += 1;
            if self.bytes_seen != self.bytes_needed {
                continue;
            }
            if self.bytes_needed == 3 {
                destination_handle.write_astral(self.code_point);
            } else {
                destination_handle.write_bmp_excl_ascii(self.code_point as u16);
            }
            self.code_point = 0;
            self.bytes_needed = 0;
            self.bytes_seen = 0;
            continue;
        },
        self,
        src_consumed,
        dest,
        source,
        b,
        destination_handle,
        unread_handle,
        check_space_astral
    );
}

#[cfg_attr(feature = "cargo-clippy", allow(never_loop))]
#[inline(never)]
pub fn convert_utf16_to_utf8_partial_inner(src: &[u16], dst: &mut [u8]) -> (usize, usize) {
    let mut read = 0;
    let mut written = 0;
    'outer: loop {
        let mut unit = {
            let src_remaining = &src[read..];
            let dst_remaining = &mut dst[written..];
            let length = if dst_remaining.len() < src_remaining.len() {
                dst_remaining.len()
            } else {
                src_remaining.len()
            };
            match unsafe {
                basic_latin_to_ascii(src_remaining.as_ptr(), dst_remaining.as_mut_ptr(), length)
            } {
                None => {
                    read += length;
                    written += length;
                    return (read, written);
                }
                Some((non_ascii, consumed)) => {
                    read += consumed;
                    written += consumed;
                    non_ascii
                }
            }
        };
        'inner: loop {
            loop {
                if written.checked_add(4).unwrap() > dst.len() {
                    return (read, written);
                }
                read += 1;
                if unit < 0x800 {
                    unsafe {
                        *(dst.get_unchecked_mut(written)) = (unit >> 6) as u8 | 0xC0u8;
                        written += 1;
                        *(dst.get_unchecked_mut(written)) = (unit & 0x3F) as u8 | 0x80u8;
                        written += 1;
                    }
                    break;
                }
                let unit_minus_surrogate_start = unit.wrapping_sub(0xD800);
                if likely(unit_minus_surrogate_start > (0xDFFF - 0xD800)) {
                    unsafe {
                        *(dst.get_unchecked_mut(written)) = (unit >> 12) as u8 | 0xE0u8;
                        written += 1;
                        *(dst.get_unchecked_mut(written)) = ((unit & 0xFC0) >> 6) as u8 | 0x80u8;
                        written += 1;
                        *(dst.get_unchecked_mut(written)) = (unit & 0x3F) as u8 | 0x80u8;
                        written += 1;
                    }
                    break;
                }
                if likely(unit_minus_surrogate_start <= (0xDBFF - 0xD800)) {
                    if read >= src.len() {
                        debug_assert_eq!(read, src.len());
                        unsafe {
                            *(dst.get_unchecked_mut(written)) = 0xEFu8;
                            written += 1;
                            *(dst.get_unchecked_mut(written)) = 0xBFu8;
                            written += 1;
                            *(dst.get_unchecked_mut(written)) = 0xBDu8;
                            written += 1;
                        }
                        return (read, written);
                    }
                    let second = src[read];
                    let second_minus_low_surrogate_start = second.wrapping_sub(0xDC00);
                    if likely(second_minus_low_surrogate_start <= (0xDFFF - 0xDC00)) {
                        read += 1;
                        let astral = (u32::from(unit) << 10) + u32::from(second)
                            - (((0xD800u32 << 10) - 0x10000u32) + 0xDC00u32);
                        unsafe {
                            *(dst.get_unchecked_mut(written)) = (astral >> 18) as u8 | 0xF0u8;
                            written += 1;
                            *(dst.get_unchecked_mut(written)) =
                                ((astral & 0x3F000u32) >> 12) as u8 | 0x80u8;
                            written += 1;
                            *(dst.get_unchecked_mut(written)) =
                                ((astral & 0xFC0u32) >> 6) as u8 | 0x80u8;
                            written += 1;
                            *(dst.get_unchecked_mut(written)) = (astral & 0x3F) as u8 | 0x80u8;
                            written += 1;
                        }
                        break;
                    }
                }
                unsafe {
                    *(dst.get_unchecked_mut(written)) = 0xEFu8;
                    written += 1;
                    *(dst.get_unchecked_mut(written)) = 0xBFu8;
                    written += 1;
                    *(dst.get_unchecked_mut(written)) = 0xBDu8;
                    written += 1;
                }
                break;
            }
            if read >= src.len() {
                debug_assert_eq!(read, src.len());
                return (read, written);
            }
            unit = src[read];
            if unlikely(unit < 0x80) {
                if written >= dst.len() {
                    debug_assert_eq!(written, dst.len());
                    return (read, written);
                }
                dst[written] = unit as u8;
                read += 1;
                written += 1;
                continue 'outer;
            }
            continue 'inner;
        }
    }
}

#[inline(never)]
pub fn convert_utf16_to_utf8_partial_tail(src: &[u16], dst: &mut [u8]) -> (usize, usize) {
    let mut read = 0;
    let mut written = 0;
    let mut unit = src[read];
    if unit < 0x800 {
        loop {
            if unit < 0x80 {
                if written >= dst.len() {
                    return (read, written);
                }
                read += 1;
                dst[written] = unit as u8;
                written += 1;
            } else if unit < 0x800 {
                if written + 2 > dst.len() {
                    return (read, written);
                }
                read += 1;
                dst[written] = (unit >> 6) as u8 | 0xC0u8;
                written += 1;
                dst[written] = (unit & 0x3F) as u8 | 0x80u8;
                written += 1;
            } else {
                return (read, written);
            }
            if read >= src.len() {
                debug_assert_eq!(read, src.len());
                return (read, written);
            }
            unit = src[read];
        }
    }
    if written + 3 > dst.len() {
        return (read, written);
    }
    read += 1;
    let unit_minus_surrogate_start = unit.wrapping_sub(0xD800);
    if unit_minus_surrogate_start <= (0xDFFF - 0xD800) {
        if unit_minus_surrogate_start <= (0xDBFF - 0xD800) {
            if read >= src.len() {
                unit = 0xFFFD;
            } else {
                let second = src[read];
                if in_inclusive_range16(second, 0xDC00, 0xDFFF) {
                    read -= 1;
                    return (read, written);
                }
                unit = 0xFFFD;
            }
        } else {
            unit = 0xFFFD;
        }
    }
    dst[written] = (unit >> 12) as u8 | 0xE0u8;
    written += 1;
    dst[written] = ((unit & 0xFC0) >> 6) as u8 | 0x80u8;
    written += 1;
    dst[written] = (unit & 0x3F) as u8 | 0x80u8;
    written += 1;
    debug_assert_eq!(written, dst.len());
    (read, written)
}

pub struct Utf8Encoder;

impl Utf8Encoder {
    pub fn new(encoding: &'static Encoding) -> Encoder {
        Encoder::new(encoding, VariantEncoder::Utf8(Utf8Encoder))
    }

    pub fn max_buffer_length_from_utf16_without_replacement(
        &self,
        u16_length: usize,
    ) -> Option<usize> {
        u16_length.checked_mul(3)
    }

    pub fn max_buffer_length_from_utf8_without_replacement(
        &self,
        byte_length: usize,
    ) -> Option<usize> {
        Some(byte_length)
    }

    pub fn encode_from_utf16_raw(
        &mut self,
        src: &[u16],
        dst: &mut [u8],
        _last: bool,
    ) -> (EncoderResult, usize, usize) {
        let (read, written) = convert_utf16_to_utf8_partial(src, dst);
        (
            if read == src.len() {
                EncoderResult::InputEmpty
            } else {
                EncoderResult::OutputFull
            },
            read,
            written,
        )
    }

    pub fn encode_from_utf8_raw(
        &mut self,
        src: &str,
        dst: &mut [u8],
        _last: bool,
    ) -> (EncoderResult, usize, usize) {
        let bytes = src.as_bytes();
        let mut to_write = bytes.len();
        if to_write <= dst.len() {
            (&mut dst[..to_write]).copy_from_slice(bytes);
            return (EncoderResult::InputEmpty, to_write, to_write);
        }
        to_write = dst.len();
        while (bytes[to_write] & 0xC0) == 0x80 {
            to_write -= 1;
        }
        (&mut dst[..to_write]).copy_from_slice(&bytes[..to_write]);
        (EncoderResult::OutputFull, to_write, to_write)
    }
}

// Any copyright to the test code below this comment is dedicated to the
