// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

pub fn value_of_lowest_1_bit(t: u32) -> u32 {
    t & t.wrapping_neg()
}
