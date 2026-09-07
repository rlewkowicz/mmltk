// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use jxl_macros::UnconditionalCoder;

use crate::{
    bit_reader::BitReader,
    error::{Error, Result},
    headers::{encodings::*, frame_header::PermutationNonserialized},
};

use super::permutation::Permutation;

pub struct TocNonserialized {
    pub num_entries: u32,
}

#[derive(UnconditionalCoder, Debug, PartialEq)]
#[nonserialized(TocNonserialized)]
pub struct Toc {
    #[default(false)]
    pub permuted: bool,

    #[default(Permutation::default())]
    #[nonserialized(num_entries: nonserialized.num_entries, permuted: permuted)]
    pub permutation: Permutation,

    #[coder(u2S(Bits(10), Bits(14) + 1024, Bits(22) + 17408, Bits(30) + 4211712))]
    #[size_coder(explicit(nonserialized.num_entries))]
    pub entries: Vec<u32>,
}

#[derive(Debug)]
pub struct IncrementalTocReader {
    num_entries: u32,
    permuted: bool,
    permutation: Option<Permutation>,
    entries: Vec<u32>,
}

impl IncrementalTocReader {
    pub fn new(num_entries: u32, br: &mut BitReader) -> Result<Self> {
        let permuted = bool::read_unconditional(&(), br, &Empty {})?;
        let mut entries = Vec::new();
        entries.try_reserve(num_entries as usize)?;
        Ok(Self {
            num_entries,
            permuted,
            permutation: None,
            entries,
        })
    }

    pub fn num_read_entries(&self) -> u32 {
        self.entries.len() as u32
    }

    pub fn remaining_entries(&self) -> u32 {
        self.num_entries - self.entries.len() as u32
    }

    pub fn is_complete(&self) -> bool {
        self.permutation.is_some() && self.remaining_entries() == 0
    }

    pub fn read_step(&mut self, br: &mut BitReader) -> Result<()> {
        if self.permutation.is_none() {
            return self.read_permutation(br);
        }

        let entry_coder = U32Coder::Select(
            U32::Bits(10),
            U32::BitsOffset { n: 14, off: 1024 },
            U32::BitsOffset { n: 22, off: 17408 },
            U32::BitsOffset {
                n: 30,
                off: 4211712,
            },
        );
        let entry = u32::read_unconditional(&entry_coder, br, &Empty {})?;
        self.entries.push(entry);
        Ok(())
    }

    fn read_permutation(&mut self, br: &mut BitReader) -> Result<()> {
        let permutation = Permutation::read_unconditional(
            &(),
            br,
            &PermutationNonserialized {
                num_entries: self.num_entries,
                permuted: self.permuted,
            },
        )?;
        self.permutation = Some(permutation);
        Ok(())
    }

    pub fn finalize(self) -> Toc {
        assert!(self.is_complete());
        let permutation = self.permutation.unwrap();
        Toc {
            permuted: self.permuted,
            permutation,
            entries: self.entries,
        }
    }
}
