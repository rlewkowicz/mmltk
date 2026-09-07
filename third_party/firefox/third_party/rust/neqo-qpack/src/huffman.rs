// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use crate::{
    Error, Res,
    huffman_decode_helper::{HuffmanDecoderNode, huffman_decoder_root},
    huffman_table::HUFFMAN_TABLE,
};

struct BitReader<'a> {
    input: &'a [u8],
    offset: usize,
    current_bit: u8,
}

impl<'a> BitReader<'a> {
    pub const fn new(input: &'a [u8]) -> Self {
        BitReader {
            input,
            offset: 0,
            current_bit: 8,
        }
    }

    pub fn read_bit(&mut self) -> Res<u8> {
        if self.input.len() == self.offset {
            return Err(Error::NeedMoreData);
        }

        if self.current_bit == 0 {
            self.offset += 1;
            if self.offset == self.input.len() {
                return Err(Error::NeedMoreData);
            }
            self.current_bit = 8;
        }
        self.current_bit -= 1;
        Ok((self.input[self.offset] >> self.current_bit) & 0x01)
    }

    pub fn verify_ending(&mut self, i: u8) -> Res<()> {
        if (i + self.current_bit) > 7 {
            return Err(Error::HuffmanDecompression);
        }

        if self.input.is_empty() {
            Ok(())
        } else if self.offset != self.input.len() {
            Err(Error::HuffmanDecompression)
        } else if self.input[self.input.len() - 1] & ((0x1 << (i + self.current_bit)) - 1)
            == ((0x1 << (i + self.current_bit)) - 1)
        {
            self.current_bit = 0;
            Ok(())
        } else {
            Err(Error::HuffmanDecompression)
        }
    }

    pub const fn has_more_data(&self) -> bool {
        !self.input.is_empty() && (self.offset != self.input.len() || (self.current_bit != 0))
    }
}

/// Decodes huffman encoded input.
///
/// # Errors
///
/// This function may return `Error::HuffmanDecompression` if `input` is not a correct
/// huffman-encoded array of bits.
///
/// # Panics
///
/// Never, but rust can't know that.
pub fn decode(input: &[u8]) -> Res<Vec<u8>> {
    let mut reader = BitReader::new(input);
    let mut output = Vec::with_capacity(input.len() * 2); 
    while reader.has_more_data() {
        if let Some(c) = decode_character(&mut reader)? {
            output.push(u8::try_from(c).map_err(|_| Error::HuffmanDecompression)?);
        }
    }

    Ok(output)
}

fn decode_character(reader: &mut BitReader) -> Res<Option<u16>> {
    let mut node: &HuffmanDecoderNode = huffman_decoder_root();
    let mut i = 0;
    while node.value.is_none() {
        match reader.read_bit() {
            Err(_) => {
                reader.verify_ending(i)?;
                return Ok(None);
            }
            Ok(b) => {
                i += 1;
                if let Some(next) = &node.next[usize::from(b)] {
                    node = next;
                } else {
                    reader.verify_ending(i)?;
                    return Ok(None);
                }
            }
        }
    }
    debug_assert!(node.value.is_some());
    Ok(node.value)
}

/// # Panics
///
/// Never, but rust doesn't know that.
#[must_use]
pub fn encode(input: &[u8]) -> Vec<u8> {
    let mut output: Vec<u8> = Vec::with_capacity(input.len()); 
    let mut left: u8 = 8;
    let mut saved: u8 = 0;
    for c in input {
        let mut e = HUFFMAN_TABLE[*c as usize];

        if e.len < left {
            let b = (e.val & 0xFF) as u8; 
            saved |= b << (left - e.len);
            left -= e.len;
            e.len = 0;
        } else {
            let v: u8 = u8::try_from(e.val >> (e.len - left)).expect("fits into u8");
            saved |= v;
            output.push(saved);
            e.len -= left;
            left = 8;
            saved = 0;
        }

        while e.len >= 8 {
            let v: u8 = ((e.val >> (e.len - 8)) & 0xFF) as u8; 
            output.push(v);
            e.len -= 8;
        }

        if e.len > 0 {
            saved = u8::try_from(e.val & ((1 << e.len) - 1)).expect("fits into u8") << (8 - e.len);
            left = 8 - e.len;
        }
    }

    if left < 8 {
        let v: u8 = (1 << left) - 1;
        saved |= v;
        output.push(saved);
    }

    output
}
