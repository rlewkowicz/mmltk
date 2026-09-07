// Copyright Mozilla Foundation. See the COPYRIGHT
// file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use super::*;
use crate::data::*;
use crate::handles::*;
use crate::variant::*;
use super::in_inclusive_range32;

pub struct Big5Decoder {
    lead: Option<u8>,
}

impl Big5Decoder {
    pub fn new() -> VariantDecoder {
        VariantDecoder::Big5(Big5Decoder { lead: None })
    }

    pub fn in_neutral_state(&self) -> bool {
        self.lead.is_none()
    }

    fn plus_one_if_lead(&self, byte_length: usize) -> Option<usize> {
        byte_length.checked_add(match self.lead {
            None => 0,
            Some(_) => 1,
        })
    }

    pub fn max_utf16_buffer_length(&self, byte_length: usize) -> Option<usize> {
        checked_add(1, self.plus_one_if_lead(byte_length))
    }

    pub fn max_utf8_buffer_length_without_replacement(&self, byte_length: usize) -> Option<usize> {
        checked_add(2, checked_mul(2, self.plus_one_if_lead(byte_length)))
    }

    pub fn max_utf8_buffer_length(&self, byte_length: usize) -> Option<usize> {
        checked_add(3, checked_mul(3, self.plus_one_if_lead(byte_length)))
    }

    ascii_compatible_two_byte_decoder_functions!(
        {
            let non_ascii_minus_offset =
                non_ascii.wrapping_sub(0x81);
            if non_ascii_minus_offset > (0xFE - 0x81) {
                return (DecoderResult::Malformed(1, 0),
                        source.consumed(),
                        handle.written());
            }
            non_ascii_minus_offset
        },
        {
            let mut trail_minus_offset =
                byte.wrapping_sub(0x40);
            if trail_minus_offset > (0x7E - 0x40) {
                let trail_minus_range_start =
                    byte.wrapping_sub(0xA1);
                if trail_minus_range_start >
                   (0xFE - 0xA1) {
                    if byte < 0x80 {
                        return (DecoderResult::Malformed(1, 0),
                                unread_handle_trail.unread(),
                                handle.written());
                    }
                    return (DecoderResult::Malformed(2, 0),
                            unread_handle_trail.consumed(),
                            handle.written());
                }
                trail_minus_offset = byte - 0x62;
            }
            let pointer = lead_minus_offset as usize *
                          157usize +
                          trail_minus_offset as usize;
            let rebased_pointer = pointer.wrapping_sub(942);
            let low_bits = big5_low_bits(rebased_pointer);
            if low_bits == 0 {
                match pointer {
                    1133 => {
                        handle.write_big5_combination(0x00CAu16,
                                                      0x0304u16)
                    }
                    1135 => {
                        handle.write_big5_combination(0x00CAu16,
                                                      0x030Cu16)
                    }
                    1164 => {
                        handle.write_big5_combination(0x00EAu16,
                                                      0x0304u16)
                    }
                    1166 => {
                        handle.write_big5_combination(0x00EAu16,
                                                      0x030Cu16)
                    }
                    _ => {
                        if byte < 0x80 {
                            return (DecoderResult::Malformed(1, 0),
                                    unread_handle_trail.unread(),
                                    handle.written());
                        }
                        return (DecoderResult::Malformed(2, 0),
                                unread_handle_trail.consumed(),
                                handle.written());
                    }
                }
            } else if big5_is_astral(rebased_pointer) {
                handle.write_astral(u32::from(low_bits) |
                                    0x20000u32)
            } else {
                handle.write_bmp_excl_ascii(low_bits)
            }
        },
        self,
        non_ascii,
        byte,
        lead_minus_offset,
        unread_handle_trail,
        source,
        handle,
        'outermost,
        copy_ascii_from_check_space_astral,
        check_space_astral,
        false);
}

pub struct Big5Encoder;

impl Big5Encoder {
    pub fn new(encoding: &'static Encoding) -> Encoder {
        Encoder::new(encoding, VariantEncoder::Big5(Big5Encoder))
    }

    pub fn max_buffer_length_from_utf16_without_replacement(
        &self,
        u16_length: usize,
    ) -> Option<usize> {
        u16_length.checked_mul(2)
    }

    pub fn max_buffer_length_from_utf8_without_replacement(
        &self,
        byte_length: usize,
    ) -> Option<usize> {
        byte_length.checked_add(1)
    }

    ascii_compatible_encoder_functions!(
        {
            if let Some((lead, trail)) = big5_level1_hanzi_encode(bmp) {
                handle.write_two(lead, trail)
            } else {
                let pointer = if let Some(pointer) = big5_box_encode(bmp) {
                    pointer
                } else if let Some(pointer) = big5_other_encode(bmp) {
                    pointer
                } else {
                    return (
                        EncoderResult::unmappable_from_bmp(bmp),
                        source.consumed(),
                        handle.written(),
                    );
                };
                let lead = pointer / 157 + 0x81;
                let remainder = pointer % 157;
                let trail = if remainder < 0x3F {
                    remainder + 0x40
                } else {
                    remainder + 0x62
                };
                handle.write_two(lead as u8, trail as u8)
            }
        },
        {
            if in_inclusive_range32(astral as u32, 0x2008A, 0x2F8A6) {
                if let Some(rebased_pointer) = big5_astral_encode(astral as u16) {
                    let lead = rebased_pointer / 157 + 0x87;
                    let remainder = rebased_pointer % 157;
                    let trail = if remainder < 0x3F {
                        remainder + 0x40
                    } else {
                        remainder + 0x62
                    };
                    handle.write_two(lead as u8, trail as u8)
                } else {
                    return (
                        EncoderResult::Unmappable(astral),
                        source.consumed(),
                        handle.written(),
                    );
                }
            } else {
                return (
                    EncoderResult::Unmappable(astral),
                    source.consumed(),
                    handle.written(),
                );
            }
        },
        bmp,
        astral,
        self,
        source,
        handle,
        copy_ascii_to_check_space_two,
        check_space_two,
        false
    );
}

// Any copyright to the test code below this comment is dedicated to the
