// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use crate::{huffman, prefix::Prefix};

/// Extension trait providing QPACK-specific encoding methods for `Encoder`.
///
/// This trait extends the standard [`neqo_common::Encoder`] with QPACK-specific
/// methods for encoding integers with prefixes and literal strings with
/// optional Huffman encoding.
pub trait Encoder {
    /// Encode an integer with a QPACK prefix according to RFC 7541 Section 5.1.
    fn encode_prefixed_encoded_int(&mut self, prefix: Prefix, val: u64) -> usize;

    /// Encode a literal string with optional Huffman encoding according to RFC 7541 Section 5.2.
    fn encode_literal(&mut self, use_huffman: bool, prefix: Prefix, value: &[u8]);
}

impl<B> Encoder for neqo_common::Encoder<B>
where
    B: neqo_common::Buffer,
{
    fn encode_prefixed_encoded_int(&mut self, prefix: Prefix, mut val: u64) -> usize {
        let first_byte_max: u8 = if prefix.len() == 0 {
            0xff
        } else {
            (1 << (8 - prefix.len())) - 1
        };

        if val < u64::from(first_byte_max) {
            let v = u8::try_from(val).expect("first_byte_max is a u8 and val is smaller");
            self.encode_byte((prefix.prefix() & !first_byte_max) | v);
            return 1;
        }

        self.encode_byte(prefix.prefix() | first_byte_max);
        val -= u64::from(first_byte_max);

        let mut written = 1;
        let mut done = false;
        while !done {
            let mut b = (val & 0x7f) as u8; 
            val >>= 7;
            if val > 0 {
                b |= 0x80;
            } else {
                done = true;
            }

            self.encode_byte(b);
            written += 1;
        }
        written
    }

    fn encode_literal(&mut self, use_huffman: bool, prefix: Prefix, value: &[u8]) {
        let real_prefix = Prefix::new(
            if use_huffman {
                prefix.prefix() | (0x80 >> prefix.len())
            } else {
                prefix.prefix()
            },
            prefix.len() + 1,
        );

        if use_huffman {
            let encoded = huffman::encode(value);
            self.encode_prefixed_encoded_int(
                real_prefix,
                u64::try_from(encoded.len()).expect("usize fits in u64"),
            );
            self.encode(&encoded);
        } else {
            self.encode_prefixed_encoded_int(
                real_prefix,
                u64::try_from(value.len()).expect("usize fits in u64"),
            );
            self.encode(value);
        }
    }
}
