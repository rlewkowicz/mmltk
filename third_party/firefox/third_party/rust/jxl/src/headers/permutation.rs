// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use std::borrow::Cow;

use crate::bit_reader::BitReader;
use crate::entropy_coding::decode::Histograms;
use crate::entropy_coding::decode::SymbolReader;
use crate::error::{Error, Result};
use crate::util::{CeilLog2, NewWithCapacity, tracing_wrappers::instrument, value_of_lowest_1_bit};

#[derive(Debug, PartialEq, Default, Clone)]
pub struct Permutation(pub Cow<'static, [u32]>);

impl std::ops::Deref for Permutation {
    type Target = [u32];

    fn deref(&self) -> &[u32] {
        &self.0
    }
}

impl Permutation {
    /// Decode a permutation from entropy-coded stream.
    pub fn decode(
        size: u32,
        skip: u32,
        histograms: &Histograms,
        br: &mut BitReader,
        entropy_reader: &mut SymbolReader,
    ) -> Result<Self> {
        let end = entropy_reader.read_unsigned(histograms, br, get_context(size));
        Self::decode_inner(size, skip, end, |ctx| -> Result<u32> {
            let r = entropy_reader.read_unsigned(histograms, br, ctx);
            br.check_for_error()?;
            Ok(r)
        })
    }

    fn decode_inner(
        size: u32,
        skip: u32,
        end: u32,
        mut read: impl FnMut(usize) -> Result<u32>,
    ) -> Result<Self> {
        if end > size - skip {
            return Err(Error::InvalidPermutationSize { size, skip, end });
        }

        let mut lehmer = Vec::new_with_capacity(end as usize)?;

        let mut prev_val = 0u32;
        for idx in skip..(skip + end) {
            let val = match read(get_context(prev_val)) {
                Ok(val) => val,
                Err(Error::OutOfBounds(_)) => {
                    let bits = (((skip + end) - idx) as usize).saturating_mul(3) / 2;
                    return Err(Error::OutOfBounds(bits));
                }
                Err(e) => return Err(e),
            };
            if val >= size - idx {
                return Err(Error::InvalidPermutationLehmerCode {
                    size,
                    idx,
                    lehmer: val,
                });
            }
            lehmer.push(val);
            prev_val = val;
        }

        let mut permutation = Vec::new_with_capacity((size - skip) as usize)?;
        permutation.extend(0..size);

        let permuted_slice = decode_lehmer_code(&lehmer, &permutation[skip as usize..])?;

        permutation[skip as usize..].copy_from_slice(&permuted_slice);

        assert_eq!(permutation.len(), size as usize);

        Ok(Self(Cow::Owned(permutation)))
    }

    pub fn compose(&mut self, other: &Permutation) {
        assert_eq!(self.0.len(), other.0.len());
        let mut tmp: Vec<u32> = vec![0; self.0.len()];
        for (i, val) in tmp.iter_mut().enumerate().take(self.0.len()) {
            *val = self.0[other.0[i] as usize]
        }
        self.0.to_mut().copy_from_slice(&tmp[..]);
    }
}

#[instrument(level = "debug", ret, err)]
fn decode_lehmer_code(code: &[u32], permutation_slice: &[u32]) -> Result<Vec<u32>> {
    let n = permutation_slice.len();
    if n == 0 {
        return Err(Error::InvalidPermutationLehmerCode {
            size: 0,
            idx: 0,
            lehmer: 0,
        });
    }

    let mut permuted = Vec::new_with_capacity(n)?;
    permuted.extend_from_slice(permutation_slice);

    let padded_n = (n as u32).next_power_of_two() as usize;

    let mut temp = Vec::new_with_capacity(padded_n)?;
    temp.extend((0..padded_n as u32).map(|x| value_of_lowest_1_bit(x + 1)));

    for (i, permuted_item) in permuted.iter_mut().enumerate() {
        let code_i = *code.get(i).unwrap_or(&0);

        if code_i as usize > n - i - 1 {
            return Err(Error::InvalidPermutationLehmerCode {
                size: n as u32,
                idx: i as u32,
                lehmer: code_i,
            });
        }

        let mut rank = code_i + 1;

        let mut bit = padded_n;
        let mut next = 0usize;
        while bit != 0 {
            let cand = next + bit;
            if cand == 0 || cand > padded_n {
                return Err(Error::InvalidPermutationLehmerCode {
                    size: n as u32,
                    idx: i as u32,
                    lehmer: code_i,
                });
            }
            bit >>= 1;
            if temp[cand - 1] < rank {
                next = cand;
                rank -= temp[cand - 1];
            }
        }

        *permuted_item = permutation_slice[next];

        next += 1;
        while next <= padded_n {
            temp[next - 1] -= 1;
            next += value_of_lowest_1_bit(next as u32) as usize;
        }
    }

    Ok(permuted)
}


fn get_context(x: u32) -> usize {
    (x + 1).ceil_log2().min(7) as usize
}
