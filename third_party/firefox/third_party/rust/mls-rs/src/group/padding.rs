// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

/// Padding used when sending an encrypted group message.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u8)]
pub enum PaddingMode {
    /// Step function based on the size of the message being sent.
    /// The amount of padding used will increase with the size of the original
    /// message.
    #[default]
    StepFunction,
    /// Padme, which limits information leakage to O(log log M) bits while
    /// retaining an overhead of max 11.11%, defined as Algorithm 1 in
    /// https://www.petsymposium.org/2019/files/papers/issue4/popets-2019-0056.pdf.
    Padme,
    /// No padding.
    None,
}

impl PaddingMode {
    pub(super) fn padded_size(&self, content_size: usize) -> usize {
        match self {
            PaddingMode::StepFunction => {
                let blind = 1
                    << ((content_size + 1)
                        .next_power_of_two()
                        .max(256)
                        .trailing_zeros()
                        - 3);

                (content_size | (blind - 1)) + 1
            }
            PaddingMode::Padme => {
                if content_size < 2 {
                    return content_size;
                }


                let e: u32 = content_size.ilog2(); 
                let s: u32 = e.ilog2() + 1; 
                let num_zero_bits: u32 = e - s; 
                let bitmask: usize = (1 << num_zero_bits) - 1; 
                (content_size + bitmask) & !bitmask 
            }
            PaddingMode::None => content_size,
        }
    }
}
