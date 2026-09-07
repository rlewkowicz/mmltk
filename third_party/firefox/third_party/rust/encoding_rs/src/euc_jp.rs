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
use super::in_inclusive_range16;

enum EucJpPending {
    None,
    Jis0208Lead(u8),
    Jis0212Shift,
    Jis0212Lead(u8),
    HalfWidthKatakana,
}

impl EucJpPending {
    fn is_none(&self) -> bool {
        match *self {
            EucJpPending::None => true,
            _ => false,
        }
    }

    fn count(&self) -> usize {
        match *self {
            EucJpPending::None => 0,
            EucJpPending::Jis0208Lead(_)
            | EucJpPending::Jis0212Shift
            | EucJpPending::HalfWidthKatakana => 1,
            EucJpPending::Jis0212Lead(_) => 2,
        }
    }
}

pub struct EucJpDecoder {
    pending: EucJpPending,
}

impl EucJpDecoder {
    pub fn new() -> VariantDecoder {
        VariantDecoder::EucJp(EucJpDecoder {
            pending: EucJpPending::None,
        })
    }

    pub fn in_neutral_state(&self) -> bool {
        self.pending.is_none()
    }

    fn plus_one_if_lead(&self, byte_length: usize) -> Option<usize> {
        byte_length.checked_add(if self.pending.is_none() { 0 } else { 1 })
    }

    pub fn max_utf16_buffer_length(&self, byte_length: usize) -> Option<usize> {
        self.plus_one_if_lead(byte_length)
    }

    pub fn max_utf8_buffer_length_without_replacement(&self, byte_length: usize) -> Option<usize> {
        let len = self.plus_one_if_lead(byte_length);
        checked_add(2, checked_add_opt(len, checked_div(checked_add(1, len), 2)))
    }

    pub fn max_utf8_buffer_length(&self, byte_length: usize) -> Option<usize> {
        checked_mul(3, self.plus_one_if_lead(byte_length))
    }

    euc_jp_decoder_functions!(
        {
            let trail_minus_offset = byte.wrapping_sub(0xA1);
            if jis0208_lead_minus_offset == 0x03 && trail_minus_offset < 0x53 {
                handle.write_upper_bmp(0x3041 + u16::from(trail_minus_offset))
            } else if jis0208_lead_minus_offset == 0x04 && trail_minus_offset < 0x56 {
                handle.write_upper_bmp(0x30A1 + u16::from(trail_minus_offset))
            } else if trail_minus_offset > (0xFE - 0xA1) {
                if byte < 0x80 {
                    return (
                        DecoderResult::Malformed(1, 0),
                        unread_handle_trail.unread(),
                        handle.written(),
                    );
                }
                return (
                    DecoderResult::Malformed(2, 0),
                    unread_handle_trail.consumed(),
                    handle.written(),
                );
            } else {
                let pointer = mul_94(jis0208_lead_minus_offset) + usize::from(trail_minus_offset);
                let level1_pointer = pointer.wrapping_sub(1410);
                if level1_pointer < JIS0208_LEVEL1_KANJI.len() {
                    handle.write_upper_bmp(JIS0208_LEVEL1_KANJI[level1_pointer])
                } else {
                    let level2_pointer = pointer.wrapping_sub(4418);
                    if level2_pointer < JIS0208_LEVEL2_AND_ADDITIONAL_KANJI.len() {
                        handle.write_upper_bmp(JIS0208_LEVEL2_AND_ADDITIONAL_KANJI[level2_pointer])
                    } else {
                        let ibm_pointer = pointer.wrapping_sub(8272);
                        if ibm_pointer < IBM_KANJI.len() {
                            handle.write_upper_bmp(IBM_KANJI[ibm_pointer])
                        } else if let Some(bmp) = jis0208_symbol_decode(pointer) {
                            handle.write_bmp_excl_ascii(bmp)
                        } else if let Some(bmp) = jis0208_range_decode(pointer) {
                            handle.write_bmp_excl_ascii(bmp)
                        } else {
                            return (
                                DecoderResult::Malformed(2, 0),
                                unread_handle_trail.consumed(),
                                handle.written(),
                            );
                        }
                    }
                }
            }
        },
        {
            let jis0212_lead_minus_offset = lead.wrapping_sub(0xA1);
            if jis0212_lead_minus_offset > (0xFE - 0xA1) {
                if lead < 0x80 {
                    return (
                        DecoderResult::Malformed(1, 0),
                        unread_handle_jis0212.unread(),
                        handle.written(),
                    );
                }
                return (
                    DecoderResult::Malformed(2, 0),
                    unread_handle_jis0212.consumed(),
                    handle.written(),
                );
            }
            jis0212_lead_minus_offset
        },
        {
            let trail_minus_offset = byte.wrapping_sub(0xA1);
            if trail_minus_offset > (0xFE - 0xA1) {
                if byte < 0x80 {
                    return (
                        DecoderResult::Malformed(2, 0),
                        unread_handle_trail.unread(),
                        handle.written(),
                    );
                }
                return (
                    DecoderResult::Malformed(3, 0),
                    unread_handle_trail.consumed(),
                    handle.written(),
                );
            }
            let pointer = mul_94(jis0212_lead_minus_offset) + usize::from(trail_minus_offset);
            let pointer_minus_kanji = pointer.wrapping_sub(1410);
            if pointer_minus_kanji < JIS0212_KANJI.len() {
                handle.write_upper_bmp(JIS0212_KANJI[pointer_minus_kanji])
            } else if let Some(bmp) = jis0212_accented_decode(pointer) {
                handle.write_bmp_excl_ascii(bmp)
            } else {
                let pointer_minus_upper_cyrillic = pointer.wrapping_sub(597);
                if pointer_minus_upper_cyrillic <= (607 - 597) {
                    handle.write_mid_bmp(0x0402 + pointer_minus_upper_cyrillic as u16)
                } else {
                    let pointer_minus_lower_cyrillic = pointer.wrapping_sub(645);
                    if pointer_minus_lower_cyrillic <= (655 - 645) {
                        handle.write_mid_bmp(0x0452 + pointer_minus_lower_cyrillic as u16)
                    } else {
                        return (
                            DecoderResult::Malformed(3, 0),
                            unread_handle_trail.consumed(),
                            handle.written(),
                        );
                    }
                }
            }
        },
        {
            let trail_minus_offset = byte.wrapping_sub(0xA1);
            if trail_minus_offset > (0xDF - 0xA1) {
                if byte < 0x80 {
                    return (
                        DecoderResult::Malformed(1, 0),
                        unread_handle_trail.unread(),
                        handle.written(),
                    );
                }
                return (
                    DecoderResult::Malformed(2, 0),
                    unread_handle_trail.consumed(),
                    handle.written(),
                );
            }
            handle.write_upper_bmp(0xFF61 + u16::from(trail_minus_offset))
        },
        self,
        non_ascii,
        jis0208_lead_minus_offset,
        byte,
        unread_handle_trail,
        jis0212_lead_minus_offset,
        lead,
        unread_handle_jis0212,
        source,
        handle
    );
}

#[cfg(feature = "fast-kanji-encode")]
#[inline(always)]
fn encode_kanji(bmp: u16) -> Option<(u8, u8)> {
    jis0208_kanji_euc_jp_encode(bmp)
}

#[cfg(not(feature = "fast-kanji-encode"))]
#[inline(always)]
fn encode_kanji(bmp: u16) -> Option<(u8, u8)> {
    if 0x4EDD == bmp {
        Some((0xA1, 0xB8))
    } else if let Some((lead, trail)) = jis0208_level1_kanji_euc_jp_encode(bmp) {
        Some((lead, trail))
    } else if let Some(pos) = jis0208_level2_and_additional_kanji_encode(bmp) {
        let lead = (pos / 94) + 0xD0;
        let trail = (pos % 94) + 0xA1;
        Some((lead as u8, trail as u8))
    } else if let Some(pos) = position(&IBM_KANJI[..], bmp) {
        let lead = (pos / 94) + 0xF9;
        let trail = (pos % 94) + 0xA1;
        Some((lead as u8, trail as u8))
    } else {
        None
    }
}

pub struct EucJpEncoder;

impl EucJpEncoder {
    pub fn new(encoding: &'static Encoding) -> Encoder {
        Encoder::new(encoding, VariantEncoder::EucJp(EucJpEncoder))
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

    ascii_compatible_bmp_encoder_functions!(
        {
            let bmp_minus_hiragana = bmp.wrapping_sub(0x3041);
            if bmp_minus_hiragana < 0x53 {
                handle.write_two(0xA4, 0xA1 + bmp_minus_hiragana as u8)
            } else if in_inclusive_range16(bmp, 0x4E00, 0x9FA0) {
                if let Some((lead, trail)) = encode_kanji(bmp) {
                    handle.write_two(lead, trail)
                } else {
                    return (
                        EncoderResult::unmappable_from_bmp(bmp),
                        source.consumed(),
                        handle.written(),
                    );
                }
            } else {
                let bmp_minus_katakana = bmp.wrapping_sub(0x30A1);
                if bmp_minus_katakana < 0x56 {
                    handle.write_two(0xA5, 0xA1 + bmp_minus_katakana as u8)
                } else {
                    let bmp_minus_space = bmp.wrapping_sub(0x3000);
                    if bmp_minus_space < 3 {
                        handle.write_two(0xA1, 0xA1 + bmp_minus_space as u8)
                    } else if bmp == 0xA5 {
                        handle.write_one(0x5Cu8)
                    } else if bmp == 0x203E {
                        handle.write_one(0x7Eu8)
                    } else if in_inclusive_range16(bmp, 0xFF61, 0xFF9F) {
                        handle.write_two(0x8Eu8, (bmp - (0xFF61 - 0xA1)) as u8)
                    } else if bmp == 0x2212 {
                        handle.write_two(0xA1u8, 0xDDu8)
                    } else if let Some(pointer) = jis0208_range_encode(bmp) {
                        let lead = (pointer / 94) + 0xA1;
                        let trail = (pointer % 94) + 0xA1;
                        handle.write_two(lead as u8, trail as u8)
                    } else if in_inclusive_range16(bmp, 0xFA0E, 0xFA2D)
                        || bmp == 0xF929
                        || bmp == 0xF9DC
                    {
                        let pos = position(&IBM_KANJI[..], bmp).unwrap();
                        let lead = (pos / 94) + 0xF9;
                        let trail = (pos % 94) + 0xA1;
                        handle.write_two(lead as u8, trail as u8)
                    } else if let Some(pointer) = ibm_symbol_encode(bmp) {
                        let lead = (pointer / 94) + 0xA1;
                        let trail = (pointer % 94) + 0xA1;
                        handle.write_two(lead as u8, trail as u8)
                    } else if let Some(pointer) = jis0208_symbol_encode(bmp) {
                        let lead = (pointer / 94) + 0xA1;
                        let trail = (pointer % 94) + 0xA1;
                        handle.write_two(lead as u8, trail as u8)
                    } else {
                        return (
                            EncoderResult::unmappable_from_bmp(bmp),
                            source.consumed(),
                            handle.written(),
                        );
                    }
                }
            }
        },
        bmp,
        self,
        source,
        handle,
        copy_ascii_to_check_space_two,
        check_space_two,
        false
    );
}

// Any copyright to the test code below this comment is dedicated to the
