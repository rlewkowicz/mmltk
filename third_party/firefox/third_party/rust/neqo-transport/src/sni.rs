// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::ops::Range;

use neqo_common::{Decoder, qtrace};

/// Finds the range where the SNI extension lives, or returns `None`.
#[must_use]
pub fn find_sni(buf: &[u8]) -> Option<Range<usize>> {
    #[must_use]
    fn skip(dec: &mut Decoder, len: usize) -> Option<()> {
        if len > dec.remaining() {
            return None;
        }
        dec.skip(len);
        Some(())
    }

    #[must_use]
    fn skip_vec<T>(dec: &mut Decoder) -> Option<()>
    where
        T: TryFrom<u64>,
        usize: TryFrom<T>,
    {
        let len = dec.decode_uint::<T>()?;
        skip(dec, usize::try_from(len).ok()?)
    }

    let mut dec = Decoder::from(buf);

    if buf.is_empty() || dec.decode_uint::<u8>()? != 1 {
        return None;
    }
    skip(&mut dec, 3 + 2 + 32)?; 
    skip_vec::<u8>(&mut dec)?; 
    skip_vec::<u16>(&mut dec)?; 
    skip_vec::<u8>(&mut dec)?; 
    skip(&mut dec, 2)?;

    while dec.remaining() >= 4 {
        let ext_type: u16 = dec.decode_uint()?;
        let ext_len: u16 = dec.decode_uint()?;
        if ext_type == 0 {
            let sni_len = dec.decode_uint::<u16>()?;
            if sni_len < 3 {
                return None;
            }
            skip(&mut dec, 3)?; 
            let start = dec.offset();
            let end = start + usize::from(sni_len) - 3;
            if end > dec.offset() + dec.remaining() {
                return None;
            }
            qtrace!(
                "SNI range {start}..{end}: {:?}",
                String::from_utf8_lossy(&buf[start..end])
            );
            return Some(start..end);
        }
        skip(&mut dec, ext_len.into())?;
    }
    None
}
