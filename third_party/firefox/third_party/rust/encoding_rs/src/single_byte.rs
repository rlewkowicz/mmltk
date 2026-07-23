// Copyright Mozilla Foundation. See the COPYRIGHT
// file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use super::*;
use crate::ascii::*;
use crate::data::position;
use crate::handles::*;
use crate::variant::*;

pub struct SingleByteDecoder {
    table: &'static [u16; 128],
}

impl SingleByteDecoder {
    pub fn new(data: &'static [u16; 128]) -> VariantDecoder {
        VariantDecoder::SingleByte(SingleByteDecoder { table: data })
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

    pub fn decode_to_utf8_raw(
        &mut self,
        src: &[u8],
        dst: &mut [u8],
        _last: bool,
    ) -> (DecoderResult, usize, usize) {
        let mut source = ByteSource::new(src);
        let mut dest = Utf8Destination::new(dst);
        'outermost: loop {
            match dest.copy_ascii_from_check_space_bmp(&mut source) {
                CopyAsciiResult::Stop(ret) => return ret,
                CopyAsciiResult::GoOn((mut non_ascii, mut handle)) => 'middle: loop {
                    let mapped =
                        unsafe { *(self.table.get_unchecked(non_ascii as usize - 0x80usize)) };
                    if mapped == 0u16 {
                        return (
                            DecoderResult::Malformed(1, 0),
                            source.consumed(),
                            handle.written(),
                        );
                    }
                    let dest_again = handle.write_bmp_excl_ascii(mapped);
                    match source.check_available() {
                        Space::Full(src_consumed) => {
                            return (
                                DecoderResult::InputEmpty,
                                src_consumed,
                                dest_again.written(),
                            );
                        }
                        Space::Available(source_handle) => {
                            match dest_again.check_space_bmp() {
                                Space::Full(dst_written) => {
                                    return (
                                        DecoderResult::OutputFull,
                                        source_handle.consumed(),
                                        dst_written,
                                    );
                                }
                                Space::Available(mut destination_handle) => {
                                    let (mut b, unread_handle) = source_handle.read();
                                    let source_again = unread_handle.commit();
                                    'innermost: loop {
                                        if b > 127 {
                                            non_ascii = b;
                                            handle = destination_handle;
                                            continue 'middle;
                                        }
                                        let dest_again_again = destination_handle.write_ascii(b);
                                        if b < 60 {
                                            match source_again.check_available() {
                                                Space::Full(src_consumed_again) => {
                                                    return (
                                                        DecoderResult::InputEmpty,
                                                        src_consumed_again,
                                                        dest_again_again.written(),
                                                    );
                                                }
                                                Space::Available(source_handle_again) => {
                                                    match dest_again_again.check_space_bmp() {
                                                        Space::Full(dst_written_again) => {
                                                            return (
                                                                DecoderResult::OutputFull,
                                                                source_handle_again.consumed(),
                                                                dst_written_again,
                                                            );
                                                        }
                                                        Space::Available(
                                                            destination_handle_again,
                                                        ) => {
                                                            let (b_again, _unread_handle_again) =
                                                                source_handle_again.read();
                                                            b = b_again;
                                                            destination_handle =
                                                                destination_handle_again;
                                                            continue 'innermost;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        continue 'outermost;
                                    }
                                }
                            }
                        }
                    }
                },
            }
        }
    }

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
        let mut converted = 0usize;
        'outermost: loop {
            match unsafe {
                ascii_to_basic_latin(
                    src.as_ptr().add(converted),
                    dst.as_mut_ptr().add(converted),
                    length - converted,
                )
            } {
                None => {
                    return (pending, length, length);
                }
                Some((mut non_ascii, consumed)) => {
                    converted += consumed;
                    'middle: loop {
                        let mapped =
                            unsafe { *(self.table.get_unchecked(non_ascii as usize - 0x80usize)) };
                        if mapped == 0u16 {
                            return (
                                DecoderResult::Malformed(1, 0),
                                converted + 1, 
                                converted,
                            );
                        }
                        unsafe {
                            *(dst.get_unchecked_mut(converted)) = mapped;
                        }
                        converted += 1;
                        if converted == length {
                            return (pending, length, length);
                        }
                        let mut b = unsafe { *(src.get_unchecked(converted)) };
                        'innermost: loop {
                            if b > 127 {
                                non_ascii = b;
                                continue 'middle;
                            }
                            unsafe {
                                *(dst.get_unchecked_mut(converted)) = u16::from(b);
                            }
                            converted += 1;
                            if b < 60 {
                                if converted == length {
                                    return (pending, length, length);
                                }
                                b = unsafe { *(src.get_unchecked(converted)) };
                                continue 'innermost;
                            }
                            continue 'outermost;
                        }
                    }
                }
            }
        }
    }

    pub fn latin1_byte_compatible_up_to(&self, buffer: &[u8]) -> usize {
        let mut bytes = buffer;
        let mut total = 0;
        loop {
            if let Some((non_ascii, offset)) = validate_ascii(bytes) {
                total += offset;
                let mapped = unsafe { *(self.table.get_unchecked(non_ascii as usize - 0x80usize)) };
                if mapped != u16::from(non_ascii) {
                    return total;
                }
                total += 1;
                bytes = &bytes[offset + 1..];
            } else {
                return total;
            }
        }
    }
}

pub struct SingleByteEncoder {
    table: &'static [u16; 128],
    run_bmp_offset: usize,
    run_byte_offset: usize,
    run_length: usize,
}

impl SingleByteEncoder {
    pub fn new(
        encoding: &'static Encoding,
        data: &'static [u16; 128],
        run_bmp_offset: u16,
        run_byte_offset: u8,
        run_length: u8,
    ) -> Encoder {
        Encoder::new(
            encoding,
            VariantEncoder::SingleByte(SingleByteEncoder {
                table: data,
                run_bmp_offset: run_bmp_offset as usize,
                run_byte_offset: run_byte_offset as usize,
                run_length: run_length as usize,
            }),
        )
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

    #[inline(always)]
    fn encode_u16(&self, code_unit: u16) -> Option<u8> {

        let unit_as_usize = code_unit as usize;
        let offset = unit_as_usize.wrapping_sub(self.run_bmp_offset);
        if offset < self.run_length {
            return Some((128 + self.run_byte_offset + offset) as u8);
        }

        let tail_start = self.run_byte_offset + self.run_length;
        if let Some(pos) = position(&self.table[tail_start..], code_unit) {
            return Some((128 + tail_start + pos) as u8);
        }

        if self.run_byte_offset >= 64 {
            if let Some(pos) = position(&self.table[64..self.run_byte_offset], code_unit) {
                return Some(((128 + 64) + pos) as u8);
            }

            if let Some(pos) = position(&self.table[32..64], code_unit) {
                return Some(((128 + 32) + pos) as u8);
            }
        } else if let Some(pos) = position(&self.table[32..self.run_byte_offset], code_unit) {
            return Some(((128 + 32) + pos) as u8);
        }

        if let Some(pos) = position(&self.table[..32], code_unit) {
            return Some((128 + pos) as u8);
        }

        None
    }

    ascii_compatible_bmp_encoder_function!(
        {
            match self.encode_u16(bmp) {
                Some(byte) => handle.write_one(byte),
                None => {
                    return (
                        EncoderResult::unmappable_from_bmp(bmp),
                        source.consumed(),
                        handle.written(),
                    );
                }
            }
        },
        bmp,
        self,
        source,
        handle,
        copy_ascii_to_check_space_one,
        check_space_one,
        encode_from_utf8_raw,
        str,
        Utf8Source,
        true
    );

    pub fn encode_from_utf16_raw(
        &mut self,
        src: &[u16],
        dst: &mut [u8],
        _last: bool,
    ) -> (EncoderResult, usize, usize) {
        let (pending, length) = if dst.len() < src.len() {
            (EncoderResult::OutputFull, dst.len())
        } else {
            (EncoderResult::InputEmpty, src.len())
        };
        let mut converted = 0usize;
        'outermost: loop {
            match unsafe {
                basic_latin_to_ascii(
                    src.as_ptr().add(converted),
                    dst.as_mut_ptr().add(converted),
                    length - converted,
                )
            } {
                None => {
                    return (pending, length, length);
                }
                Some((mut non_ascii, consumed)) => {
                    converted += consumed;
                    'middle: loop {
                        match self.encode_u16(non_ascii) {
                            Some(byte) => {
                                unsafe {
                                    *(dst.get_unchecked_mut(converted)) = byte;
                                }
                                converted += 1;
                            }
                            None => {
                                let high_bits = non_ascii & 0xFC00u16;
                                if high_bits == 0xD800u16 {
                                    if converted + 1 == length {
                                        return (
                                            EncoderResult::Unmappable('\u{FFFD}'),
                                            converted + 1, 
                                            converted,
                                        );
                                    }
                                    let second =
                                        u32::from(unsafe { *src.get_unchecked(converted + 1) });
                                    if second & 0xFC00u32 != 0xDC00u32 {
                                        return (
                                            EncoderResult::Unmappable('\u{FFFD}'),
                                            converted + 1, 
                                            converted,
                                        );
                                    }
                                    let astral: char = unsafe {
                                        ::core::char::from_u32_unchecked(
                                            (u32::from(non_ascii) << 10) + second
                                                - (((0xD800u32 << 10) - 0x1_0000u32) + 0xDC00u32),
                                        )
                                    };
                                    return (
                                        EncoderResult::Unmappable(astral),
                                        converted + 2, 
                                        converted,
                                    );
                                }
                                if high_bits == 0xDC00u16 {
                                    return (
                                        EncoderResult::Unmappable('\u{FFFD}'),
                                        converted + 1, 
                                        converted,
                                    );
                                }
                                return (
                                    EncoderResult::unmappable_from_bmp(non_ascii),
                                    converted + 1, 
                                    converted,
                                );
                            }
                        }
                        if converted == length {
                            return (pending, length, length);
                        }
                        let mut unit = unsafe { *(src.get_unchecked(converted)) };
                        'innermost: loop {
                            if unit > 127 {
                                non_ascii = unit;
                                continue 'middle;
                            }
                            unsafe {
                                *(dst.get_unchecked_mut(converted)) = unit as u8;
                            }
                            converted += 1;
                            if unit < 60 {
                                if converted == length {
                                    return (pending, length, length);
                                }
                                unit = unsafe { *(src.get_unchecked(converted)) };
                                continue 'innermost;
                            }
                            continue 'outermost;
                        }
                    }
                }
            }
        }
    }
}

// Any copyright to the test code below this comment is dedicated to the
