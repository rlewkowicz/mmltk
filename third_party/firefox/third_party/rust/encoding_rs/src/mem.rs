// Copyright Mozilla Foundation. See the COPYRIGHT
// file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! Functions for converting between different in-RAM representations of text
//! and for quickly checking if the Unicode Bidirectional Algorithm can be
//! avoided.
//!
//! By using slices for output, the functions here seek to enable by-register
//! (ALU register or SIMD register as available) operations in order to
//! outperform iterator-based conversions available in the Rust standard
//! library.
//!
//! _Note:_ "Latin1" in this module refers to the Unicode range from U+0000 to
//! U+00FF, inclusive, and does not refer to the windows-1252 range. This
//! in-memory encoding is sometimes used as a storage optimization of text
//! when UTF-16 indexing and length semantics are exposed.
//!
//! The FFI binding for this module are in the
//! [encoding_c_mem crate](https://github.com/hsivonen/encoding_c_mem).

#[cfg(feature = "alloc")]
use alloc::borrow::Cow;
#[cfg(feature = "alloc")]
use alloc::string::String;
#[cfg(feature = "alloc")]
use alloc::vec::Vec;

use super::in_inclusive_range16;
use super::in_inclusive_range32;
use super::in_inclusive_range8;
use super::in_range16;
use super::in_range32;
use super::DecoderResult;
use crate::ascii::*;
use crate::utf_8::*;

macro_rules! non_fuzz_debug_assert {
    ($($arg:tt)*) => (debug_assert!($($arg)*))
}

cfg_if! {
    if #[cfg(feature = "simd-accel")] {
        use ::core::intrinsics::likely;
        use ::core::intrinsics::unlikely;
    } else {
        #[inline(always)]
        fn likely(b: bool) -> bool {
            b
        }
        #[inline(always)]
        fn unlikely(b: bool) -> bool {
            b
        }
    }
}

/// Classification of text as Latin1 (all code points are below U+0100),
/// left-to-right with some non-Latin1 characters or as containing at least
/// some right-to-left characters.
#[must_use]
#[derive(Debug, PartialEq, Eq)]
#[repr(C)]
pub enum Latin1Bidi {
    /// Every character is below U+0100.
    Latin1 = 0,
    /// There is at least one character that's U+0100 or higher, but there
    /// are no right-to-left characters.
    LeftToRight = 1,
    /// There is at least one right-to-left character.
    Bidi = 2,
}

#[allow(dead_code)]
const LATIN1_MASK: usize = 0xFF00_FF00_FF00_FF00u64 as usize;

#[allow(unused_macros)]
macro_rules! by_unit_check_alu {
    ($name:ident, $unit:ty, $bound:expr, $mask:ident) => {
        #[cfg_attr(feature = "cargo-clippy", allow(cast_ptr_alignment))]
        #[inline(always)]
        fn $name(buffer: &[$unit]) -> bool {
            let mut offset = 0usize;
            let mut accu = 0usize;
            let unit_size = ::core::mem::size_of::<$unit>();
            let len = buffer.len();
            if len >= ALU_ALIGNMENT / unit_size {
                if buffer[0] >= $bound {
                    return false;
                }
                let src = buffer.as_ptr();
                let mut until_alignment = ((ALU_ALIGNMENT - ((src as usize) & ALU_ALIGNMENT_MASK))
                    & ALU_ALIGNMENT_MASK)
                    / unit_size;
                if until_alignment + ALU_ALIGNMENT / unit_size <= len {
                    if until_alignment != 0 {
                        accu |= buffer[offset] as usize;
                        offset += 1;
                        until_alignment -= 1;
                        while until_alignment != 0 {
                            accu |= buffer[offset] as usize;
                            offset += 1;
                            until_alignment -= 1;
                        }
                        if accu >= $bound {
                            return false;
                        }
                    }
                    let len_minus_stride = len - ALU_ALIGNMENT / unit_size;
                    if offset + (4 * (ALU_ALIGNMENT / unit_size)) <= len {
                        let len_minus_unroll = len - (4 * (ALU_ALIGNMENT / unit_size));
                        loop {
                            let unroll_accu = unsafe { *(src.add(offset) as *const usize) }
                                | unsafe {
                                    *(src.add(offset + (ALU_ALIGNMENT / unit_size)) as *const usize)
                                }
                                | unsafe {
                                    *(src.add(offset + (2 * (ALU_ALIGNMENT / unit_size)))
                                        as *const usize)
                                }
                                | unsafe {
                                    *(src.add(offset + (3 * (ALU_ALIGNMENT / unit_size)))
                                        as *const usize)
                                };
                            if unroll_accu & $mask != 0 {
                                return false;
                            }
                            offset += 4 * (ALU_ALIGNMENT / unit_size);
                            if offset > len_minus_unroll {
                                break;
                            }
                        }
                    }
                    while offset <= len_minus_stride {
                        accu |= unsafe { *(src.add(offset) as *const usize) };
                        offset += ALU_ALIGNMENT / unit_size;
                    }
                }
            }
            for &unit in &buffer[offset..] {
                accu |= unit as usize;
            }
            accu & $mask == 0
        }
    };
}

#[allow(unused_macros)]
macro_rules! by_unit_check_simd {
    ($name:ident, $unit:ty, $splat:expr, $simd_ty:ty, $bound:expr, $func:ident) => {
        #[inline(always)]
        fn $name(buffer: &[$unit]) -> bool {
            let mut offset = 0usize;
            let mut accu = 0usize;
            let unit_size = ::core::mem::size_of::<$unit>();
            let len = buffer.len();
            if len >= SIMD_STRIDE_SIZE / unit_size {
                if buffer[0] >= $bound {
                    return false;
                }
                let src = buffer.as_ptr();
                let mut until_alignment = ((SIMD_ALIGNMENT
                    - ((src as usize) & SIMD_ALIGNMENT_MASK))
                    & SIMD_ALIGNMENT_MASK)
                    / unit_size;
                if until_alignment + SIMD_STRIDE_SIZE / unit_size <= len {
                    if until_alignment != 0 {
                        accu |= buffer[offset] as usize;
                        offset += 1;
                        until_alignment -= 1;
                        while until_alignment != 0 {
                            accu |= buffer[offset] as usize;
                            offset += 1;
                            until_alignment -= 1;
                        }
                        if accu >= $bound {
                            return false;
                        }
                    }
                    let len_minus_stride = len - SIMD_STRIDE_SIZE / unit_size;
                    if offset + (4 * (SIMD_STRIDE_SIZE / unit_size)) <= len {
                        let len_minus_unroll = len - (4 * (SIMD_STRIDE_SIZE / unit_size));
                        loop {
                            let unroll_accu = unsafe { *(src.add(offset) as *const $simd_ty) }
                                | unsafe {
                                    *(src.add(offset + (SIMD_STRIDE_SIZE / unit_size))
                                        as *const $simd_ty)
                                }
                                | unsafe {
                                    *(src.add(offset + (2 * (SIMD_STRIDE_SIZE / unit_size)))
                                        as *const $simd_ty)
                                }
                                | unsafe {
                                    *(src.add(offset + (3 * (SIMD_STRIDE_SIZE / unit_size)))
                                        as *const $simd_ty)
                                };
                            if !$func(unroll_accu) {
                                return false;
                            }
                            offset += 4 * (SIMD_STRIDE_SIZE / unit_size);
                            if offset > len_minus_unroll {
                                break;
                            }
                        }
                    }
                    let mut simd_accu = $splat;
                    while offset <= len_minus_stride {
                        simd_accu = simd_accu | unsafe { *(src.add(offset) as *const $simd_ty) };
                        offset += SIMD_STRIDE_SIZE / unit_size;
                    }
                    if !$func(simd_accu) {
                        return false;
                    }
                }
            }
            for &unit in &buffer[offset..] {
                accu |= unit as usize;
            }
            accu < $bound
        }
    };
}

cfg_if! {
    if #[cfg(all(feature = "simd-accel", any(target_feature = "sse2", all(target_endian = "little", target_arch = "aarch64"), all(target_endian = "little", target_feature = "neon"))))] {
        use crate::simd_funcs::*;
        use core::simd::u8x16;
        use core::simd::u16x8;

        const SIMD_ALIGNMENT: usize = 16;

        const SIMD_ALIGNMENT_MASK: usize = 15;

        by_unit_check_simd!(is_ascii_impl, u8, u8x16::splat(0), u8x16, 0x80, simd_is_ascii);
        by_unit_check_simd!(is_basic_latin_impl, u16, u16x8::splat(0), u16x8, 0x80, simd_is_basic_latin);
        by_unit_check_simd!(is_utf16_latin1_impl, u16, u16x8::splat(0), u16x8, 0x100, simd_is_latin1);

        #[inline(always)]
        fn utf16_valid_up_to_impl(buffer: &[u16]) -> usize {
            let unit_size = ::core::mem::size_of::<u16>();
            let src = buffer.as_ptr();
            let len = buffer.len();
            let mut offset = 0usize;
            'outer: loop {
                let until_alignment = ((SIMD_ALIGNMENT - ((unsafe { src.add(offset) } as usize) & SIMD_ALIGNMENT_MASK)) &
                                        SIMD_ALIGNMENT_MASK) / unit_size;
                if until_alignment == 0 {
                    if offset + SIMD_STRIDE_SIZE / unit_size > len {
                        break;
                    }
                } else {
                    let offset_plus_until_alignment = offset + until_alignment;
                    let offset_plus_until_alignment_plus_one = offset_plus_until_alignment + 1;
                    if offset_plus_until_alignment_plus_one + SIMD_STRIDE_SIZE / unit_size > len {
                        break;
                    }
                    let (up_to, last_valid_low) = utf16_valid_up_to_alu(&buffer[offset..offset_plus_until_alignment_plus_one]);
                    if up_to < until_alignment {
                        return offset + up_to;
                    }
                    if last_valid_low {
                        offset = offset_plus_until_alignment_plus_one;
                        continue;
                    }
                    offset = offset_plus_until_alignment;
                }
                let len_minus_stride = len - SIMD_STRIDE_SIZE / unit_size;
                loop {
                    let offset_plus_stride = offset + SIMD_STRIDE_SIZE / unit_size;
                    if contains_surrogates(unsafe { *(src.add(offset) as *const u16x8) }) {
                        if offset_plus_stride == len {
                            break 'outer;
                        }
                        let offset_plus_stride_plus_one = offset_plus_stride + 1;
                        let (up_to, last_valid_low) = utf16_valid_up_to_alu(&buffer[offset..offset_plus_stride_plus_one]);
                        if up_to < SIMD_STRIDE_SIZE / unit_size {
                            return offset + up_to;
                        }
                        if last_valid_low {
                            offset = offset_plus_stride_plus_one;
                            continue 'outer;
                        }
                    }
                    offset = offset_plus_stride;
                    if offset > len_minus_stride {
                        break 'outer;
                    }
                }
            }
            let (up_to, _) = utf16_valid_up_to_alu(&buffer[offset..]);
            offset + up_to
        }
    } else {
        by_unit_check_alu!(is_ascii_impl, u8, 0x80, ASCII_MASK);
        by_unit_check_alu!(is_basic_latin_impl, u16, 0x80, BASIC_LATIN_MASK);
        by_unit_check_alu!(is_utf16_latin1_impl, u16, 0x100, LATIN1_MASK);

        #[inline(always)]
        fn utf16_valid_up_to_impl(buffer: &[u16]) -> usize {
            let (up_to, _) = utf16_valid_up_to_alu(buffer);
            up_to
        }
    }
}

/// The second return value is true iff the last code unit of the slice was
/// reached and turned out to be a low surrogate that is part of a valid pair.
#[cfg_attr(feature = "cargo-clippy", allow(collapsible_if))]
#[inline(always)]
fn utf16_valid_up_to_alu(buffer: &[u16]) -> (usize, bool) {
    let len = buffer.len();
    if len == 0 {
        return (0, false);
    }
    let mut offset = 0usize;
    loop {
        let unit = buffer[offset];
        let next = offset + 1;
        let unit_minus_surrogate_start = unit.wrapping_sub(0xD800);
        if unit_minus_surrogate_start > (0xDFFF - 0xD800) {
            offset = next;
            if offset == len {
                return (offset, false);
            }
            continue;
        }
        if unit_minus_surrogate_start <= (0xDBFF - 0xD800) {
            if next < len {
                let second = buffer[next];
                let second_minus_low_surrogate_start = second.wrapping_sub(0xDC00);
                if second_minus_low_surrogate_start <= (0xDFFF - 0xDC00) {
                    offset = next + 1;
                    if offset == len {
                        return (offset, true);
                    }
                    continue;
                }
                // fall through
            }
            // Unpaired, fall through
        }
        return (offset, false);
    }
}

cfg_if! {
    if #[cfg(all(feature = "simd-accel", any(target_feature = "sse2", all(target_endian = "little", target_arch = "aarch64"), all(target_endian = "little", target_feature = "neon"))))] {
        #[inline(always)]
        fn is_str_latin1_impl(buffer: &str) -> Option<usize> {
            let mut offset = 0usize;
            let bytes = buffer.as_bytes();
            let len = bytes.len();
            if len >= SIMD_STRIDE_SIZE {
                let src = bytes.as_ptr();
                let mut until_alignment = (SIMD_ALIGNMENT - ((src as usize) & SIMD_ALIGNMENT_MASK)) &
                                           SIMD_ALIGNMENT_MASK;
                if until_alignment + SIMD_STRIDE_SIZE <= len {
                    while until_alignment != 0 {
                        if bytes[offset] > 0xC3 {
                            return Some(offset);
                        }
                        offset += 1;
                        until_alignment -= 1;
                    }
                    let len_minus_stride = len - SIMD_STRIDE_SIZE;
                    loop {
                        if !simd_is_str_latin1(unsafe { *(src.add(offset) as *const u8x16) }) {
                            while bytes[offset] & 0xC0 == 0x80 {
                                offset += 1;
                            }
                            return Some(offset);
                        }
                        offset += SIMD_STRIDE_SIZE;
                        if offset > len_minus_stride {
                            break;
                        }
                    }
                }
            }
            for i in offset..len {
                if bytes[i] > 0xC3 {
                    return Some(i);
                }
            }
            None
        }
    } else {
        #[inline(always)]
        fn is_str_latin1_impl(buffer: &str) -> Option<usize> {
            let mut bytes = buffer.as_bytes();
            let mut total = 0;
            loop {
                if let Some((byte, offset)) = validate_ascii(bytes) {
                    total += offset;
                    if byte > 0xC3 {
                        return Some(total);
                    }
                    bytes = &bytes[offset + 2..];
                    total += 2;
                } else {
                    return None;
                }
            }
        }
    }
}

#[inline(always)]
fn is_utf8_latin1_impl(buffer: &[u8]) -> Option<usize> {
    let mut bytes = buffer;
    let mut total = 0;
    loop {
        if let Some((byte, offset)) = validate_ascii(bytes) {
            total += offset;
            if in_inclusive_range8(byte, 0xC2, 0xC3) {
                let next = offset + 1;
                if next == bytes.len() {
                    return Some(total);
                }
                if bytes[next] & 0xC0 != 0x80 {
                    return Some(total);
                }
                bytes = &bytes[offset + 2..];
                total += 2;
            } else {
                return Some(total);
            }
        } else {
            return None;
        }
    }
}

cfg_if! {
    if #[cfg(all(feature = "simd-accel", any(target_feature = "sse2", all(target_endian = "little", target_arch = "aarch64"), all(target_endian = "little", target_feature = "neon"))))] {
        #[inline(always)]
        fn is_utf16_bidi_impl(buffer: &[u16]) -> bool {
            let mut offset = 0usize;
            let len = buffer.len();
            if len >= SIMD_STRIDE_SIZE / 2 {
                let src = buffer.as_ptr();
                let mut until_alignment = ((SIMD_ALIGNMENT - ((src as usize) & SIMD_ALIGNMENT_MASK)) &
                                           SIMD_ALIGNMENT_MASK) / 2;
                if until_alignment + (SIMD_STRIDE_SIZE / 2) <= len {
                    while until_alignment != 0 {
                        if is_utf16_code_unit_bidi(buffer[offset]) {
                            return true;
                        }
                        offset += 1;
                        until_alignment -= 1;
                    }
                    let len_minus_stride = len - (SIMD_STRIDE_SIZE / 2);
                    loop {
                        if is_u16x8_bidi(unsafe { *(src.add(offset) as *const u16x8) }) {
                            return true;
                        }
                        offset += SIMD_STRIDE_SIZE / 2;
                        if offset > len_minus_stride {
                            break;
                        }
                    }
                }
            }
            for &u in &buffer[offset..] {
                if is_utf16_code_unit_bidi(u) {
                    return true;
                }
            }
            false
        }
    } else {
        #[inline(always)]
        fn is_utf16_bidi_impl(buffer: &[u16]) -> bool {
            for &u in buffer {
                if is_utf16_code_unit_bidi(u) {
                    return true;
                }
            }
            false
        }
    }
}

cfg_if! {
    if #[cfg(all(feature = "simd-accel", any(target_feature = "sse2", all(target_endian = "little", target_arch = "aarch64"), all(target_endian = "little", target_feature = "neon"))))] {
        #[inline(always)]
        fn check_utf16_for_latin1_and_bidi_impl(buffer: &[u16]) -> Latin1Bidi {
            let mut offset = 0usize;
            let len = buffer.len();
            if len >= SIMD_STRIDE_SIZE / 2 {
                let src = buffer.as_ptr();
                let mut until_alignment = ((SIMD_ALIGNMENT - ((src as usize) & SIMD_ALIGNMENT_MASK)) &
                                           SIMD_ALIGNMENT_MASK) / 2;
                if until_alignment + (SIMD_STRIDE_SIZE / 2) <= len {
                    while until_alignment != 0 {
                        if buffer[offset] > 0xFF {
                            if is_utf16_bidi_impl(&buffer[offset..]) {
                                return Latin1Bidi::Bidi;
                            }
                            return Latin1Bidi::LeftToRight;
                        }
                        offset += 1;
                        until_alignment -= 1;
                    }
                    let len_minus_stride = len - (SIMD_STRIDE_SIZE / 2);
                    loop {
                        let mut s = unsafe { *(src.add(offset) as *const u16x8) };
                        if !simd_is_latin1(s) {
                            loop {
                                if is_u16x8_bidi(s) {
                                    return Latin1Bidi::Bidi;
                                }
                                offset += SIMD_STRIDE_SIZE / 2;
                                if offset > len_minus_stride {
                                    for &u in &buffer[offset..] {
                                        if is_utf16_code_unit_bidi(u) {
                                            return Latin1Bidi::Bidi;
                                        }
                                    }
                                    return Latin1Bidi::LeftToRight;
                                }
                                s = unsafe { *(src.add(offset) as *const u16x8) };
                            }
                        }
                        offset += SIMD_STRIDE_SIZE / 2;
                        if offset > len_minus_stride {
                            break;
                        }
                    }
                }
            }
            let mut iter = (&buffer[offset..]).iter();
            loop {
                if let Some(&u) = iter.next() {
                    if u > 0xFF {
                        let mut inner_u = u;
                        loop {
                            if is_utf16_code_unit_bidi(inner_u) {
                                return Latin1Bidi::Bidi;
                            }
                            if let Some(&code_unit) = iter.next() {
                                inner_u = code_unit;
                            } else {
                                return Latin1Bidi::LeftToRight;
                            }
                        }
                    }
                } else {
                    return Latin1Bidi::Latin1;
                }
            }
        }
    } else {
        #[cfg_attr(feature = "cargo-clippy", allow(cast_ptr_alignment))]
        #[inline(always)]
        fn check_utf16_for_latin1_and_bidi_impl(buffer: &[u16]) -> Latin1Bidi {
            let mut offset = 0usize;
            let len = buffer.len();
            if len >= ALU_ALIGNMENT / 2 {
                let src = buffer.as_ptr();
                let mut until_alignment = ((ALU_ALIGNMENT - ((src as usize) & ALU_ALIGNMENT_MASK)) &
                                           ALU_ALIGNMENT_MASK) / 2;
                if until_alignment + ALU_ALIGNMENT / 2 <= len {
                    while until_alignment != 0 {
                        if buffer[offset] > 0xFF {
                            if is_utf16_bidi_impl(&buffer[offset..]) {
                                return Latin1Bidi::Bidi;
                            }
                            return Latin1Bidi::LeftToRight;
                        }
                        offset += 1;
                        until_alignment -= 1;
                    }
                    let len_minus_stride = len - ALU_ALIGNMENT / 2;
                    loop {
                        if unsafe { *(src.add(offset) as *const usize) } & LATIN1_MASK != 0 {
                            if is_utf16_bidi_impl(&buffer[offset..]) {
                                return Latin1Bidi::Bidi;
                            }
                            return Latin1Bidi::LeftToRight;
                        }
                        offset += ALU_ALIGNMENT / 2;
                        if offset > len_minus_stride {
                            break;
                        }
                    }
                }
            }
            let mut iter = (&buffer[offset..]).iter();
            loop {
                if let Some(&u) = iter.next() {
                    if u > 0xFF {
                        let mut inner_u = u;
                        loop {
                            if is_utf16_code_unit_bidi(inner_u) {
                                return Latin1Bidi::Bidi;
                            }
                            if let Some(&code_unit) = iter.next() {
                                inner_u = code_unit;
                            } else {
                                return Latin1Bidi::LeftToRight;
                            }
                        }
                    }
                } else {
                    return Latin1Bidi::Latin1;
                }
            }
        }
    }
}

/// Checks whether the buffer is all-ASCII.
///
/// May read the entire buffer even if it isn't all-ASCII. (I.e. the function
/// is not guaranteed to fail fast.)
pub fn is_ascii(buffer: &[u8]) -> bool {
    is_ascii_impl(buffer)
}

/// Checks whether the buffer is all-Basic Latin (i.e. UTF-16 representing
/// only ASCII characters).
///
/// May read the entire buffer even if it isn't all-ASCII. (I.e. the function
/// is not guaranteed to fail fast.)
pub fn is_basic_latin(buffer: &[u16]) -> bool {
    is_basic_latin_impl(buffer)
}

/// Checks whether the buffer is valid UTF-8 representing only code points
/// less than or equal to U+00FF.
///
/// Fails fast. (I.e. returns before having read the whole buffer if UTF-8
/// invalidity or code points above U+00FF are discovered.
pub fn is_utf8_latin1(buffer: &[u8]) -> bool {
    is_utf8_latin1_impl(buffer).is_none()
}

/// Checks whether the buffer represents only code points less than or equal
/// to U+00FF.
///
/// Fails fast. (I.e. returns before having read the whole buffer if code
/// points above U+00FF are discovered.
pub fn is_str_latin1(buffer: &str) -> bool {
    is_str_latin1_impl(buffer).is_none()
}

/// Checks whether the buffer represents only code point less than or equal
/// to U+00FF.
///
/// May read the entire buffer even if it isn't all-Latin1. (I.e. the function
/// is not guaranteed to fail fast.)
pub fn is_utf16_latin1(buffer: &[u16]) -> bool {
    is_utf16_latin1_impl(buffer)
}

/// Checks whether a potentially-invalid UTF-8 buffer contains code points
/// that trigger right-to-left processing.
///
/// The check is done on a Unicode block basis without regard to assigned
/// vs. unassigned code points in the block. Hebrew presentation forms in
/// the Alphabetic Presentation Forms block are treated as if they formed
/// a block on their own (i.e. it treated as right-to-left). Additionally,
/// the four RIGHT-TO-LEFT FOO controls in General Punctuation are checked
/// for. Control characters that are technically bidi controls but do not
/// cause right-to-left behavior without the presence of right-to-left
/// characters or right-to-left controls are not checked for. As a special
/// case, U+FEFF is excluded from Arabic Presentation Forms-B.
///
/// Returns `true` if the input is invalid UTF-8 or the input contains an
/// RTL character. Returns `false` if the input is valid UTF-8 and contains
/// no RTL characters.
#[cfg_attr(feature = "cargo-clippy", allow(collapsible_if, cyclomatic_complexity))]
#[inline]
pub fn is_utf8_bidi(buffer: &[u8]) -> bool {

    let mut src = buffer;
    'outer: loop {
        if let Some((mut byte, mut read)) = validate_ascii(src) {
            if read + 4 <= src.len() {
                'inner: loop {
                    match byte {
                        0..=0x7F => {
                            read += 1;
                            src = &src[read..];
                            continue 'outer;
                        }
                        0xC2..=0xD5 => {
                            let second = unsafe { *(src.get_unchecked(read + 1)) };
                            if !in_inclusive_range8(second, 0x80, 0xBF) {
                                return true;
                            }
                            read += 2;
                        }
                        0xD6 => {
                            let second = unsafe { *(src.get_unchecked(read + 1)) };
                            if !in_inclusive_range8(second, 0x80, 0xBF) {
                                return true;
                            }
                            if second > 0x8F {
                                return true;
                            }
                            read += 2;
                        }
                        0xE1 | 0xE3..=0xEC | 0xEE => {
                            let second = unsafe { *(src.get_unchecked(read + 1)) };
                            let third = unsafe { *(src.get_unchecked(read + 2)) };
                            if ((UTF8_DATA.table[usize::from(second)]
                                & unsafe {
                                    *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80))
                                })
                                | (third >> 6))
                                != 2
                            {
                                return true;
                            }
                            read += 3;
                        }
                        0xE2 => {
                            let second = unsafe { *(src.get_unchecked(read + 1)) };
                            let third = unsafe { *(src.get_unchecked(read + 2)) };
                            if ((UTF8_DATA.table[usize::from(second)]
                                & unsafe {
                                    *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80))
                                })
                                | (third >> 6))
                                != 2
                            {
                                return true;
                            }
                            if second == 0x80 {
                                if third == 0x8F || third == 0xAB || third == 0xAE {
                                    return true;
                                }
                            } else if second == 0x81 {
                                if third == 0xA7 {
                                    return true;
                                }
                            }
                            read += 3;
                        }
                        0xEF => {
                            let second = unsafe { *(src.get_unchecked(read + 1)) };
                            let third = unsafe { *(src.get_unchecked(read + 2)) };
                            if ((UTF8_DATA.table[usize::from(second)]
                                & unsafe {
                                    *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80))
                                })
                                | (third >> 6))
                                != 2
                            {
                                return true;
                            }
                            if in_inclusive_range8(second, 0xAC, 0xB7) {
                                if second == 0xAC {
                                    if third > 0x9C {
                                        return true;
                                    }
                                } else {
                                    return true;
                                }
                            } else if in_inclusive_range8(second, 0xB9, 0xBB) {
                                if second == 0xB9 {
                                    if third > 0xAF {
                                        return true;
                                    }
                                } else if second == 0xBB {
                                    if third != 0xBF {
                                        return true;
                                    }
                                } else {
                                    return true;
                                }
                            }
                            read += 3;
                        }
                        0xE0 => {
                            let second = unsafe { *(src.get_unchecked(read + 1)) };
                            let third = unsafe { *(src.get_unchecked(read + 2)) };
                            if ((UTF8_DATA.table[usize::from(second)]
                                & unsafe {
                                    *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80))
                                })
                                | (third >> 6))
                                != 2
                            {
                                return true;
                            }
                            if second < 0xA4 {
                                return true;
                            }
                            read += 3;
                        }
                        0xED => {
                            let second = unsafe { *(src.get_unchecked(read + 1)) };
                            let third = unsafe { *(src.get_unchecked(read + 2)) };
                            if ((UTF8_DATA.table[usize::from(second)]
                                & unsafe {
                                    *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80))
                                })
                                | (third >> 6))
                                != 2
                            {
                                return true;
                            }
                            read += 3;
                        }
                        0xF1..=0xF4 => {
                            let second = unsafe { *(src.get_unchecked(read + 1)) };
                            let third = unsafe { *(src.get_unchecked(read + 2)) };
                            let fourth = unsafe { *(src.get_unchecked(read + 3)) };
                            if (u16::from(
                                UTF8_DATA.table[usize::from(second)]
                                    & unsafe {
                                        *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80))
                                    },
                            ) | u16::from(third >> 6)
                                | (u16::from(fourth & 0xC0) << 2))
                                != 0x202
                            {
                                return true;
                            }
                            read += 4;
                        }
                        0xF0 => {
                            let second = unsafe { *(src.get_unchecked(read + 1)) };
                            let third = unsafe { *(src.get_unchecked(read + 2)) };
                            let fourth = unsafe { *(src.get_unchecked(read + 3)) };
                            if (u16::from(
                                UTF8_DATA.table[usize::from(second)]
                                    & unsafe {
                                        *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80))
                                    },
                            ) | u16::from(third >> 6)
                                | (u16::from(fourth & 0xC0) << 2))
                                != 0x202
                            {
                                return true;
                            }
                            if unlikely(second == 0x90 || second == 0x9E) {
                                let third = src[read + 2];
                                if third >= 0xA0 {
                                    return true;
                                }
                            }
                            read += 4;
                        }
                        _ => {
                            return true;
                        }
                    }
                    if read + 4 > src.len() {
                        if read == src.len() {
                            return false;
                        }
                        byte = src[read];
                        break 'inner;
                    }
                    byte = src[read];
                    continue 'inner;
                }
            }

            match byte {
                0..=0x7F => {
                    read += 1;
                    src = &src[read..];
                    continue 'outer;
                }
                0xC2..=0xD5 => {
                    let new_read = read + 2;
                    if new_read > src.len() {
                        return true;
                    }
                    let second = unsafe { *(src.get_unchecked(read + 1)) };
                    if !in_inclusive_range8(second, 0x80, 0xBF) {
                        return true;
                    }
                    read = new_read;
                    src = &src[read..];
                    continue 'outer;
                }
                0xD6 => {
                    let new_read = read + 2;
                    if new_read > src.len() {
                        return true;
                    }
                    let second = unsafe { *(src.get_unchecked(read + 1)) };
                    if !in_inclusive_range8(second, 0x80, 0xBF) {
                        return true;
                    }
                    if second > 0x8F {
                        return true;
                    }
                    read = new_read;
                    src = &src[read..];
                    continue 'outer;
                }
                0xE1 | 0xE3..=0xEC | 0xEE => {
                    let new_read = read + 3;
                    if new_read > src.len() {
                        return true;
                    }
                    let second = unsafe { *(src.get_unchecked(read + 1)) };
                    let third = unsafe { *(src.get_unchecked(read + 2)) };
                    if ((UTF8_DATA.table[usize::from(second)]
                        & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) })
                        | (third >> 6))
                        != 2
                    {
                        return true;
                    }
                }
                0xE2 => {
                    let new_read = read + 3;
                    if new_read > src.len() {
                        return true;
                    }
                    let second = unsafe { *(src.get_unchecked(read + 1)) };
                    let third = unsafe { *(src.get_unchecked(read + 2)) };
                    if ((UTF8_DATA.table[usize::from(second)]
                        & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) })
                        | (third >> 6))
                        != 2
                    {
                        return true;
                    }
                    if second == 0x80 {
                        if third == 0x8F || third == 0xAB || third == 0xAE {
                            return true;
                        }
                    } else if second == 0x81 {
                        if third == 0xA7 {
                            return true;
                        }
                    }
                }
                0xEF => {
                    let new_read = read + 3;
                    if new_read > src.len() {
                        return true;
                    }
                    let second = unsafe { *(src.get_unchecked(read + 1)) };
                    let third = unsafe { *(src.get_unchecked(read + 2)) };
                    if ((UTF8_DATA.table[usize::from(second)]
                        & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) })
                        | (third >> 6))
                        != 2
                    {
                        return true;
                    }
                    if in_inclusive_range8(second, 0xAC, 0xB7) {
                        if second == 0xAC {
                            if third > 0x9C {
                                return true;
                            }
                        } else {
                            return true;
                        }
                    } else if in_inclusive_range8(second, 0xB9, 0xBB) {
                        if second == 0xB9 {
                            if third > 0xAF {
                                return true;
                            }
                        } else if second == 0xBB {
                            if third != 0xBF {
                                return true;
                            }
                        } else {
                            return true;
                        }
                    }
                }
                0xE0 => {
                    let new_read = read + 3;
                    if new_read > src.len() {
                        return true;
                    }
                    let second = unsafe { *(src.get_unchecked(read + 1)) };
                    let third = unsafe { *(src.get_unchecked(read + 2)) };
                    if ((UTF8_DATA.table[usize::from(second)]
                        & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) })
                        | (third >> 6))
                        != 2
                    {
                        return true;
                    }
                    if second < 0xA4 {
                        return true;
                    }
                }
                0xED => {
                    let new_read = read + 3;
                    if new_read > src.len() {
                        return true;
                    }
                    let second = unsafe { *(src.get_unchecked(read + 1)) };
                    let third = unsafe { *(src.get_unchecked(read + 2)) };
                    if ((UTF8_DATA.table[usize::from(second)]
                        & unsafe { *(UTF8_DATA.table.get_unchecked(byte as usize + 0x80)) })
                        | (third >> 6))
                        != 2
                    {
                        return true;
                    }
                }
                _ => {
                    return true;
                }
            }
            return false;
        } else {
            return false;
        }
    }
}

/// Checks whether a valid UTF-8 buffer contains code points that trigger
/// right-to-left processing.
///
/// The check is done on a Unicode block basis without regard to assigned
/// vs. unassigned code points in the block. Hebrew presentation forms in
/// the Alphabetic Presentation Forms block are treated as if they formed
/// a block on their own (i.e. it treated as right-to-left). Additionally,
/// the four RIGHT-TO-LEFT FOO controls in General Punctuation are checked
/// for. Control characters that are technically bidi controls but do not
/// cause right-to-left behavior without the presence of right-to-left
/// characters or right-to-left controls are not checked for. As a special
/// case, U+FEFF is excluded from Arabic Presentation Forms-B.
#[cfg_attr(feature = "cargo-clippy", allow(collapsible_if))]
#[inline]
pub fn is_str_bidi(buffer: &str) -> bool {
    let mut bytes = buffer.as_bytes();
    'outer: loop {
        if let Some((mut byte, mut read)) = validate_ascii(bytes) {
            'inner: loop {
                if byte < 0xE0 {
                    if byte >= 0x80 {
                        if unlikely(byte >= 0xD6) {
                            if byte == 0xD6 {
                                let second = bytes[read + 1];
                                if second > 0x8F {
                                    return true;
                                }
                            } else {
                                return true;
                            }
                        }
                        read += 2;
                    } else {
                        read += 1;
                        bytes = &bytes[read..];
                        continue 'outer;
                    }
                } else if byte < 0xF0 {
                    if unlikely(!in_inclusive_range8(byte, 0xE3, 0xEE) && byte != 0xE1) {
                        let second = bytes[read + 1];
                        if byte == 0xE0 {
                            if second < 0xA4 {
                                return true;
                            }
                        } else if byte == 0xE2 {
                            let third = bytes[read + 2];
                            if second == 0x80 {
                                if third == 0x8F || third == 0xAB || third == 0xAE {
                                    return true;
                                }
                            } else if second == 0x81 {
                                if third == 0xA7 {
                                    return true;
                                }
                            }
                        } else {
                            debug_assert_eq!(byte, 0xEF);
                            if in_inclusive_range8(second, 0xAC, 0xB7) {
                                if second == 0xAC {
                                    let third = bytes[read + 2];
                                    if third > 0x9C {
                                        return true;
                                    }
                                } else {
                                    return true;
                                }
                            } else if in_inclusive_range8(second, 0xB9, 0xBB) {
                                if second == 0xB9 {
                                    let third = bytes[read + 2];
                                    if third > 0xAF {
                                        return true;
                                    }
                                } else if second == 0xBB {
                                    let third = bytes[read + 2];
                                    if third != 0xBF {
                                        return true;
                                    }
                                } else {
                                    return true;
                                }
                            }
                        }
                    }
                    read += 3;
                } else {
                    let second = bytes[read + 1];
                    if unlikely(byte == 0xF0 && (second == 0x90 || second == 0x9E)) {
                        let third = bytes[read + 2];
                        if third >= 0xA0 {
                            return true;
                        }
                    }
                    read += 4;
                }
                if read >= bytes.len() {
                    return false;
                }
                byte = bytes[read];
                continue 'inner;
            }
        } else {
            return false;
        }
    }
}

/// Checks whether a UTF-16 buffer contains code points that trigger
/// right-to-left processing.
///
/// The check is done on a Unicode block basis without regard to assigned
/// vs. unassigned code points in the block. Hebrew presentation forms in
/// the Alphabetic Presentation Forms block are treated as if they formed
/// a block on their own (i.e. it treated as right-to-left). Additionally,
/// the four RIGHT-TO-LEFT FOO controls in General Punctuation are checked
/// for. Control characters that are technically bidi controls but do not
/// cause right-to-left behavior without the presence of right-to-left
/// characters or right-to-left controls are not checked for. As a special
/// case, U+FEFF is excluded from Arabic Presentation Forms-B.
///
/// Returns `true` if the input contains an RTL character or an unpaired
/// high surrogate that could be the high half of an RTL character.
/// Returns `false` if the input contains neither RTL characters nor
/// unpaired high surrogates that could be higher halves of RTL characters.
pub fn is_utf16_bidi(buffer: &[u16]) -> bool {
    is_utf16_bidi_impl(buffer)
}

/// Checks whether a scalar value triggers right-to-left processing.
///
/// The check is done on a Unicode block basis without regard to assigned
/// vs. unassigned code points in the block. Hebrew presentation forms in
/// the Alphabetic Presentation Forms block are treated as if they formed
/// a block on their own (i.e. it treated as right-to-left). Additionally,
/// the four RIGHT-TO-LEFT FOO controls in General Punctuation are checked
/// for. Control characters that are technically bidi controls but do not
/// cause right-to-left behavior without the presence of right-to-left
/// characters or right-to-left controls are not checked for. As a special
/// case, U+FEFF is excluded from Arabic Presentation Forms-B.
#[inline(always)]
pub fn is_char_bidi(c: char) -> bool {
    let code_point = u32::from(c);
    if code_point < 0x0590 {
        return false;
    }
    if in_range32(code_point, 0x0900, 0xFB1D) {
        if in_inclusive_range32(code_point, 0x200F, 0x2067) {
            return code_point == 0x200F
                || code_point == 0x202B
                || code_point == 0x202E
                || code_point == 0x2067;
        }
        return false;
    }
    if code_point > 0x1EFFF {
        return false;
    }
    if in_range32(code_point, 0x11000, 0x1E800) {
        return false;
    }
    if in_range32(code_point, 0xFEFF, 0x10800) {
        return false;
    }
    if in_range32(code_point, 0xFE00, 0xFE70) {
        return false;
    }
    true
}

/// Checks whether a UTF-16 code unit triggers right-to-left processing.
///
/// The check is done on a Unicode block basis without regard to assigned
/// vs. unassigned code points in the block. Hebrew presentation forms in
/// the Alphabetic Presentation Forms block are treated as if they formed
/// a block on their own (i.e. it treated as right-to-left). Additionally,
/// the four RIGHT-TO-LEFT FOO controls in General Punctuation are checked
/// for. Control characters that are technically bidi controls but do not
/// cause right-to-left behavior without the presence of right-to-left
/// characters or right-to-left controls are not checked for. As a special
/// case, U+FEFF is excluded from Arabic Presentation Forms-B.
///
/// Since supplementary-plane right-to-left blocks are identifiable from the
/// high surrogate without examining the low surrogate, this function returns
/// `true` for such high surrogates making the function suitable for handling
/// supplementary-plane text without decoding surrogate pairs to scalar
/// values. Obviously, such high surrogates are then reported as right-to-left
/// even if actually unpaired.
#[inline(always)]
pub fn is_utf16_code_unit_bidi(u: u16) -> bool {
    if u < 0x0590 {
        return false;
    }
    if in_range16(u, 0x0900, 0xD802) {
        if in_inclusive_range16(u, 0x200F, 0x2067) {
            return u == 0x200F || u == 0x202B || u == 0x202E || u == 0x2067;
        }
        return false;
    }
    if in_range16(u, 0xD83C, 0xFB1D) {
        return false;
    }
    if in_range16(u, 0xD804, 0xD83A) {
        return false;
    }
    if u > 0xFEFE {
        return false;
    }
    if in_range16(u, 0xFE00, 0xFE70) {
        return false;
    }
    true
}

/// Checks whether a potentially invalid UTF-8 buffer contains code points
/// that trigger right-to-left processing or is all-Latin1.
///
/// Possibly more efficient than performing the checks separately.
///
/// Returns `Latin1Bidi::Latin1` if `is_utf8_latin1()` would return `true`.
/// Otherwise, returns `Latin1Bidi::Bidi` if `is_utf8_bidi()` would return
/// `true`. Otherwise, returns `Latin1Bidi::LeftToRight`.
pub fn check_utf8_for_latin1_and_bidi(buffer: &[u8]) -> Latin1Bidi {
    if let Some(offset) = is_utf8_latin1_impl(buffer) {
        if is_utf8_bidi(&buffer[offset..]) {
            Latin1Bidi::Bidi
        } else {
            Latin1Bidi::LeftToRight
        }
    } else {
        Latin1Bidi::Latin1
    }
}

/// Checks whether a valid UTF-8 buffer contains code points
/// that trigger right-to-left processing or is all-Latin1.
///
/// Possibly more efficient than performing the checks separately.
///
/// Returns `Latin1Bidi::Latin1` if `is_str_latin1()` would return `true`.
/// Otherwise, returns `Latin1Bidi::Bidi` if `is_str_bidi()` would return
/// `true`. Otherwise, returns `Latin1Bidi::LeftToRight`.
pub fn check_str_for_latin1_and_bidi(buffer: &str) -> Latin1Bidi {
    if let Some(offset) = is_str_latin1_impl(buffer) {
        if is_str_bidi(&buffer[offset..]) {
            Latin1Bidi::Bidi
        } else {
            Latin1Bidi::LeftToRight
        }
    } else {
        Latin1Bidi::Latin1
    }
}

/// Checks whether a potentially invalid UTF-16 buffer contains code points
/// that trigger right-to-left processing or is all-Latin1.
///
/// Possibly more efficient than performing the checks separately.
///
/// Returns `Latin1Bidi::Latin1` if `is_utf16_latin1()` would return `true`.
/// Otherwise, returns `Latin1Bidi::Bidi` if `is_utf16_bidi()` would return
/// `true`. Otherwise, returns `Latin1Bidi::LeftToRight`.
pub fn check_utf16_for_latin1_and_bidi(buffer: &[u16]) -> Latin1Bidi {
    check_utf16_for_latin1_and_bidi_impl(buffer)
}

/// Converts potentially-invalid UTF-8 to valid UTF-16 with errors replaced
/// with the REPLACEMENT CHARACTER.
///
/// The length of the destination buffer must be at least the length of the
/// source buffer _plus one_.
///
/// Returns the number of `u16`s written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
pub fn convert_utf8_to_utf16(src: &[u8], dst: &mut [u16]) -> usize {
    assert!(dst.len() > src.len());
    let mut decoder = Utf8Decoder::new_inner();
    let mut total_read = 0usize;
    let mut total_written = 0usize;
    loop {
        let (result, read, written) =
            decoder.decode_to_utf16_raw(&src[total_read..], &mut dst[total_written..], true);
        total_read += read;
        total_written += written;
        match result {
            DecoderResult::InputEmpty => {
                return total_written;
            }
            DecoderResult::OutputFull => {
                unreachable!("The assert at the top of the function should have caught this.");
            }
            DecoderResult::Malformed(_, _) => {
                dst[total_written] = 0xFFFD;
                total_written += 1;
            }
        }
    }
}

/// Converts valid UTF-8 to valid UTF-16.
///
/// The length of the destination buffer must be at least the length of the
/// source buffer.
///
/// Returns the number of `u16`s written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
pub fn convert_str_to_utf16(src: &str, dst: &mut [u16]) -> usize {
    assert!(
        dst.len() >= src.len(),
        "Destination must not be shorter than the source."
    );
    let bytes = src.as_bytes();
    let mut read = 0;
    let mut written = 0;
    'outer: loop {
        let mut byte = {
            let src_remaining = &bytes[read..];
            let dst_remaining = &mut dst[written..];
            let length = src_remaining.len();
            match unsafe {
                ascii_to_basic_latin(src_remaining.as_ptr(), dst_remaining.as_mut_ptr(), length)
            } {
                None => {
                    written += length;
                    return written;
                }
                Some((non_ascii, consumed)) => {
                    read += consumed;
                    written += consumed;
                    non_ascii
                }
            }
        };
        'inner: loop {
            if byte < 0xE0 {
                if byte >= 0x80 {
                    let second = unsafe { *(bytes.get_unchecked(read + 1)) };
                    let point = ((u16::from(byte) & 0x1F) << 6) | (u16::from(second) & 0x3F);
                    unsafe { *(dst.get_unchecked_mut(written)) = point };
                    read += 2;
                    written += 1;
                } else {
                    unsafe { *(dst.get_unchecked_mut(written)) = u16::from(byte) };
                    read += 1;
                    written += 1;
                    continue 'outer;
                }
            } else if byte < 0xF0 {
                let second = unsafe { *(bytes.get_unchecked(read + 1)) };
                let third = unsafe { *(bytes.get_unchecked(read + 2)) };
                let point = ((u16::from(byte) & 0xF) << 12)
                    | ((u16::from(second) & 0x3F) << 6)
                    | (u16::from(third) & 0x3F);
                unsafe { *(dst.get_unchecked_mut(written)) = point };
                read += 3;
                written += 1;
            } else {
                let second = unsafe { *(bytes.get_unchecked(read + 1)) };
                let third = unsafe { *(bytes.get_unchecked(read + 2)) };
                let fourth = unsafe { *(bytes.get_unchecked(read + 3)) };
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
            }
            if read >= src.len() {
                return written;
            }
            byte = bytes[read];
            continue 'inner;
        }
    }
}

/// Converts potentially-invalid UTF-8 to valid UTF-16 signaling on error.
///
/// The length of the destination buffer must be at least the length of the
/// source buffer.
///
/// Returns the number of `u16`s written or `None` if the input was invalid.
///
/// When the input was invalid, some output may have been written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
pub fn convert_utf8_to_utf16_without_replacement(src: &[u8], dst: &mut [u16]) -> Option<usize> {
    assert!(
        dst.len() >= src.len(),
        "Destination must not be shorter than the source."
    );
    let (read, written) = convert_utf8_to_utf16_up_to_invalid(src, dst);
    if read == src.len() {
        return Some(written);
    }
    None
}

/// Converts potentially-invalid UTF-16 to valid UTF-8 with errors replaced
/// with the REPLACEMENT CHARACTER with potentially insufficient output
/// space.
///
/// Returns the number of code units read and the number of bytes written.
///
/// Guarantees that the bytes in the destination beyond the number of
/// bytes claimed as written by the second item of the return tuple
/// are left unmodified.
///
/// Not all code units are read if there isn't enough output space.
///
/// Note  that this method isn't designed for general streamability but for
/// not allocating memory for the worst case up front. Specifically,
/// if the input starts with or ends with an unpaired surrogate, those are
/// replaced with the REPLACEMENT CHARACTER.
///
/// Matches the semantics of `TextEncoder.encodeInto()` from the
/// Encoding Standard.
///
/// # Safety
///
/// If you want to convert into a `&mut str`, use
/// `convert_utf16_to_str_partial()` instead of using this function
/// together with the `unsafe` method `as_bytes_mut()` on `&mut str`.
#[inline(always)]
pub fn convert_utf16_to_utf8_partial(src: &[u16], dst: &mut [u8]) -> (usize, usize) {
    let (read, written) = convert_utf16_to_utf8_partial_inner(src, dst);
    if likely(read == src.len()) {
        return (read, written);
    }
    let (tail_read, tail_written) =
        convert_utf16_to_utf8_partial_tail(&src[read..], &mut dst[written..]);
    (read + tail_read, written + tail_written)
}

/// Converts potentially-invalid UTF-16 to valid UTF-8 with errors replaced
/// with the REPLACEMENT CHARACTER.
///
/// The length of the destination buffer must be at least the length of the
/// source buffer times three.
///
/// Returns the number of bytes written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
///
/// # Safety
///
/// If you want to convert into a `&mut str`, use `convert_utf16_to_str()`
/// instead of using this function together with the `unsafe` method
/// `as_bytes_mut()` on `&mut str`.
#[inline(always)]
pub fn convert_utf16_to_utf8(src: &[u16], dst: &mut [u8]) -> usize {
    assert!(dst.len() >= src.len() * 3);
    let (read, written) = convert_utf16_to_utf8_partial(src, dst);
    debug_assert_eq!(read, src.len());
    written
}

/// Converts potentially-invalid UTF-16 to valid UTF-8 with errors replaced
/// with the REPLACEMENT CHARACTER such that the validity of the output is
/// signaled using the Rust type system with potentially insufficient output
/// space.
///
/// Returns the number of code units read and the number of bytes written.
///
/// Not all code units are read if there isn't enough output space.
///
/// Note  that this method isn't designed for general streamability but for
/// not allocating memory for the worst case up front. Specifically,
/// if the input starts with or ends with an unpaired surrogate, those are
/// replaced with the REPLACEMENT CHARACTER.
pub fn convert_utf16_to_str_partial(src: &[u16], dst: &mut str) -> (usize, usize) {
    let bytes: &mut [u8] = unsafe { dst.as_bytes_mut() };
    let (read, written) = convert_utf16_to_utf8_partial(src, bytes);
    let len = bytes.len();
    let mut trail = written;
    while trail < len && ((bytes[trail] & 0xC0) == 0x80) {
        bytes[trail] = 0;
        trail += 1;
    }
    (read, written)
}

/// Converts potentially-invalid UTF-16 to valid UTF-8 with errors replaced
/// with the REPLACEMENT CHARACTER such that the validity of the output is
/// signaled using the Rust type system.
///
/// The length of the destination buffer must be at least the length of the
/// source buffer times three.
///
/// Returns the number of bytes written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
#[inline(always)]
pub fn convert_utf16_to_str(src: &[u16], dst: &mut str) -> usize {
    assert!(dst.len() >= src.len() * 3);
    let (read, written) = convert_utf16_to_str_partial(src, dst);
    debug_assert_eq!(read, src.len());
    written
}

/// Converts bytes whose unsigned value is interpreted as Unicode code point
/// (i.e. U+0000 to U+00FF, inclusive) to UTF-16.
///
/// The length of the destination buffer must be at least the length of the
/// source buffer.
///
/// The number of `u16`s written equals the length of the source buffer.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
pub fn convert_latin1_to_utf16(src: &[u8], dst: &mut [u16]) {
    assert!(
        dst.len() >= src.len(),
        "Destination must not be shorter than the source."
    );
    unsafe {
        unpack_latin1(src.as_ptr(), dst.as_mut_ptr(), src.len());
    }
}

/// Converts bytes whose unsigned value is interpreted as Unicode code point
/// (i.e. U+0000 to U+00FF, inclusive) to UTF-8 with potentially insufficient
/// output space.
///
/// Returns the number of bytes read and the number of bytes written.
///
/// If the output isn't large enough, not all input is consumed.
///
/// # Safety
///
/// If you want to convert into a `&mut str`, use
/// `convert_utf16_to_str_partial()` instead of using this function
/// together with the `unsafe` method `as_bytes_mut()` on `&mut str`.
pub fn convert_latin1_to_utf8_partial(src: &[u8], dst: &mut [u8]) -> (usize, usize) {
    let src_len = src.len();
    let src_ptr = src.as_ptr();
    let dst_ptr = dst.as_mut_ptr();
    let dst_len = dst.len();
    let mut total_read = 0usize;
    let mut total_written = 0usize;
    loop {
        let src_left = src_len - total_read;
        let dst_left = dst_len - total_written;
        let min_left = ::core::cmp::min(src_left, dst_left);
        if let Some((non_ascii, consumed)) = unsafe {
            ascii_to_ascii(
                src_ptr.add(total_read),
                dst_ptr.add(total_written),
                min_left,
            )
        } {
            total_read += consumed;
            total_written += consumed;
            if total_written.checked_add(2).unwrap() > dst_len {
                return (total_read, total_written);
            }

            total_read += 1; 

            dst[total_written] = (non_ascii >> 6) | 0xC0;
            total_written += 1;
            dst[total_written] = (non_ascii & 0x3F) | 0x80;
            total_written += 1;
            continue;
        }
        return (total_read + min_left, total_written + min_left);
    }
}

/// Converts bytes whose unsigned value is interpreted as Unicode code point
/// (i.e. U+0000 to U+00FF, inclusive) to UTF-8.
///
/// The length of the destination buffer must be at least the length of the
/// source buffer times two.
///
/// Returns the number of bytes written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
///
/// # Safety
///
/// Note that this function may write garbage beyond the number of bytes
/// indicated by the return value, so using a `&mut str` interpreted as
/// `&mut [u8]` as the destination is not safe. If you want to convert into
/// a `&mut str`, use `convert_utf16_to_str()` instead of this function.
#[inline]
pub fn convert_latin1_to_utf8(src: &[u8], dst: &mut [u8]) -> usize {
    assert!(
        dst.len() >= src.len() * 2,
        "Destination must not be shorter than the source times two."
    );
    let (read, written) = convert_latin1_to_utf8_partial(src, dst);
    debug_assert_eq!(read, src.len());
    written
}

/// Converts bytes whose unsigned value is interpreted as Unicode code point
/// (i.e. U+0000 to U+00FF, inclusive) to UTF-8 such that the validity of the
/// output is signaled using the Rust type system with potentially insufficient
/// output space.
///
/// Returns the number of bytes read and the number of bytes written.
///
/// If the output isn't large enough, not all input is consumed.
#[inline]
pub fn convert_latin1_to_str_partial(src: &[u8], dst: &mut str) -> (usize, usize) {
    let bytes: &mut [u8] = unsafe { dst.as_bytes_mut() };
    let (read, written) = convert_latin1_to_utf8_partial(src, bytes);
    let len = bytes.len();
    let mut trail = written;
    let max = ::core::cmp::min(len, trail + MAX_STRIDE_SIZE);
    while trail < max {
        bytes[trail] = 0;
        trail += 1;
    }
    while trail < len && ((bytes[trail] & 0xC0) == 0x80) {
        bytes[trail] = 0;
        trail += 1;
    }
    (read, written)
}

/// Converts bytes whose unsigned value is interpreted as Unicode code point
/// (i.e. U+0000 to U+00FF, inclusive) to UTF-8 such that the validity of the
/// output is signaled using the Rust type system.
///
/// The length of the destination buffer must be at least the length of the
/// source buffer times two.
///
/// Returns the number of bytes written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
#[inline]
pub fn convert_latin1_to_str(src: &[u8], dst: &mut str) -> usize {
    assert!(
        dst.len() >= src.len() * 2,
        "Destination must not be shorter than the source times two."
    );
    let (read, written) = convert_latin1_to_str_partial(src, dst);
    debug_assert_eq!(read, src.len());
    written
}

/// If the input is valid UTF-8 representing only Unicode code points from
/// U+0000 to U+00FF, inclusive, converts the input into output that
/// represents the value of each code point as the unsigned byte value of
/// each output byte.
///
/// If the input does not fulfill the condition stated above, this function
/// panics if debug assertions are enabled (and fuzzing isn't) and otherwise
/// does something that is memory-safe without any promises about any
/// properties of the output. In particular, callers shouldn't assume the
/// output to be the same across crate versions or CPU architectures and
/// should not assume that non-ASCII input can't map to ASCII output.
///
/// The length of the destination buffer must be at least the length of the
/// source buffer.
///
/// Returns the number of bytes written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
///
/// If debug assertions are enabled (and not fuzzing) and the input is
/// not in the range U+0000 to U+00FF, inclusive.
pub fn convert_utf8_to_latin1_lossy(src: &[u8], dst: &mut [u8]) -> usize {
    assert!(
        dst.len() >= src.len(),
        "Destination must not be shorter than the source."
    );
    non_fuzz_debug_assert!(is_utf8_latin1(src));
    let src_len = src.len();
    let src_ptr = src.as_ptr();
    let dst_ptr = dst.as_mut_ptr();
    let mut total_read = 0usize;
    let mut total_written = 0usize;
    loop {
        let src_left = src_len - total_read;
        if let Some((non_ascii, consumed)) = unsafe {
            ascii_to_ascii(
                src_ptr.add(total_read),
                dst_ptr.add(total_written),
                src_left,
            )
        } {
            total_read += consumed + 1;
            total_written += consumed;

            if total_read == src_len {
                return total_written;
            }

            let trail = src[total_read];
            total_read += 1;

            dst[total_written] = ((non_ascii & 0x1F) << 6) | (trail & 0x3F);
            total_written += 1;
            continue;
        }
        return total_written + src_left;
    }
}

/// If the input is valid UTF-16 representing only Unicode code points from
/// U+0000 to U+00FF, inclusive, converts the input into output that
/// represents the value of each code point as the unsigned byte value of
/// each output byte.
///
/// If the input does not fulfill the condition stated above, does something
/// that is memory-safe without any promises about any properties of the
/// output and will probably assert in debug builds in future versions.
/// In particular, callers shouldn't assume the output to be the same across
/// crate versions or CPU architectures and should not assume that non-ASCII
/// input can't map to ASCII output.
///
/// The length of the destination buffer must be at least the length of the
/// source buffer.
///
/// The number of bytes written equals the length of the source buffer.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
///
/// (Probably in future versions if debug assertions are enabled (and not
/// fuzzing) and the input is not in the range U+0000 to U+00FF, inclusive.)
pub fn convert_utf16_to_latin1_lossy(src: &[u16], dst: &mut [u8]) {
    assert!(
        dst.len() >= src.len(),
        "Destination must not be shorter than the source."
    );
    unsafe {
        pack_latin1(src.as_ptr(), dst.as_mut_ptr(), src.len());
    }
}

/// Converts bytes whose unsigned value is interpreted as Unicode code point
/// (i.e. U+0000 to U+00FF, inclusive) to UTF-8.
///
/// Borrows if input is ASCII-only. Performs a single heap allocation
/// otherwise.
///
/// Only available if the `alloc` feature is enabled (enabled by default).
#[cfg(feature = "alloc")]
pub fn decode_latin1<'a>(bytes: &'a [u8]) -> Cow<'a, str> {
    let up_to = ascii_valid_up_to(bytes);
    if up_to >= bytes.len() {
        debug_assert_eq!(up_to, bytes.len());
        let s: &str = unsafe { ::core::str::from_utf8_unchecked(bytes) };
        return Cow::Borrowed(s);
    }
    let (head, tail) = bytes.split_at(up_to);
    let capacity = head.len() + tail.len() * 2;
    let mut vec = Vec::with_capacity(capacity);
    unsafe {
        vec.set_len(capacity);
    }
    (&mut vec[..up_to]).copy_from_slice(head);
    let written = convert_latin1_to_utf8(tail, &mut vec[up_to..]);
    vec.truncate(up_to + written);
    Cow::Owned(unsafe { String::from_utf8_unchecked(vec) })
}

/// If the input is valid UTF-8 representing only Unicode code points from
/// U+0000 to U+00FF, inclusive, converts the input into output that
/// represents the value of each code point as the unsigned byte value of
/// each output byte.
///
/// If the input does not fulfill the condition stated above, this function
/// panics if debug assertions are enabled (and fuzzing isn't) and otherwise
/// does something that is memory-safe without any promises about any
/// properties of the output. In particular, callers shouldn't assume the
/// output to be the same across crate versions or CPU architectures and
/// should not assume that non-ASCII input can't map to ASCII output.
///
/// Borrows if input is ASCII-only. Performs a single heap allocation
/// otherwise.
///
/// Only available if the `alloc` feature is enabled (enabled by default).
#[cfg(feature = "alloc")]
pub fn encode_latin1_lossy<'a>(string: &'a str) -> Cow<'a, [u8]> {
    let bytes = string.as_bytes();
    let up_to = ascii_valid_up_to(bytes);
    if up_to >= bytes.len() {
        debug_assert_eq!(up_to, bytes.len());
        return Cow::Borrowed(bytes);
    }
    let (head, tail) = bytes.split_at(up_to);
    let capacity = bytes.len();
    let mut vec = Vec::with_capacity(capacity);
    unsafe {
        vec.set_len(capacity);
    }
    (&mut vec[..up_to]).copy_from_slice(head);
    let written = convert_utf8_to_latin1_lossy(tail, &mut vec[up_to..]);
    vec.truncate(up_to + written);
    Cow::Owned(vec)
}

/// Returns the index of the first unpaired surrogate or, if the input is
/// valid UTF-16 in its entirety, the length of the input.
pub fn utf16_valid_up_to(buffer: &[u16]) -> usize {
    utf16_valid_up_to_impl(buffer)
}

/// Returns the index of first byte that starts an invalid byte
/// sequence or a non-Latin1 byte sequence, or the length of the
/// string if there are neither.
pub fn utf8_latin1_up_to(buffer: &[u8]) -> usize {
    is_utf8_latin1_impl(buffer).unwrap_or(buffer.len())
}

/// Returns the index of first byte that starts a non-Latin1 byte
/// sequence, or the length of the string if there are none.
pub fn str_latin1_up_to(buffer: &str) -> usize {
    is_str_latin1_impl(buffer).unwrap_or_else(|| buffer.len())
}

/// Replaces unpaired surrogates in the input with the REPLACEMENT CHARACTER.
#[inline]
pub fn ensure_utf16_validity(buffer: &mut [u16]) {
    let mut offset = 0;
    loop {
        offset += utf16_valid_up_to(&buffer[offset..]);
        if offset == buffer.len() {
            return;
        }
        buffer[offset] = 0xFFFD;
        offset += 1;
    }
}

/// Copies ASCII from source to destination up to the first non-ASCII byte
/// (or the end of the input if it is ASCII in its entirety).
///
/// The length of the destination buffer must be at least the length of the
/// source buffer.
///
/// Returns the number of bytes written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
pub fn copy_ascii_to_ascii(src: &[u8], dst: &mut [u8]) -> usize {
    assert!(
        dst.len() >= src.len(),
        "Destination must not be shorter than the source."
    );
    if let Some((_, consumed)) =
        unsafe { ascii_to_ascii(src.as_ptr(), dst.as_mut_ptr(), src.len()) }
    {
        consumed
    } else {
        src.len()
    }
}

/// Copies ASCII from source to destination zero-extending it to UTF-16 up to
/// the first non-ASCII byte (or the end of the input if it is ASCII in its
/// entirety).
///
/// The length of the destination buffer must be at least the length of the
/// source buffer.
///
/// Returns the number of `u16`s written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
pub fn copy_ascii_to_basic_latin(src: &[u8], dst: &mut [u16]) -> usize {
    assert!(
        dst.len() >= src.len(),
        "Destination must not be shorter than the source."
    );
    if let Some((_, consumed)) =
        unsafe { ascii_to_basic_latin(src.as_ptr(), dst.as_mut_ptr(), src.len()) }
    {
        consumed
    } else {
        src.len()
    }
}

/// Copies Basic Latin from source to destination narrowing it to ASCII up to
/// the first non-Basic Latin code unit (or the end of the input if it is
/// Basic Latin in its entirety).
///
/// The length of the destination buffer must be at least the length of the
/// source buffer.
///
/// Returns the number of bytes written.
///
/// # Panics
///
/// Panics if the destination buffer is shorter than stated above.
pub fn copy_basic_latin_to_ascii(src: &[u16], dst: &mut [u8]) -> usize {
    assert!(
        dst.len() >= src.len(),
        "Destination must not be shorter than the source."
    );
    if let Some((_, consumed)) =
        unsafe { basic_latin_to_ascii(src.as_ptr(), dst.as_mut_ptr(), src.len()) }
    {
        consumed
    } else {
        src.len()
    }
}

// Any copyright to the test code below this comment is dedicated to the
