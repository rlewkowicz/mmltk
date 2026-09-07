// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

/// Mirror-reflects a value v to fit in a [0; s) range.
#[inline(always)]
pub fn mirror(mut v: isize, s: usize) -> usize {
    loop {
        if v < 0 {
            v = -v - 1;
        } else if v >= s as isize {
            v = s as isize * 2 - v - 1;
        } else {
            return v as usize;
        }
    }
}
