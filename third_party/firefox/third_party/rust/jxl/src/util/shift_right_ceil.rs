// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use std::ops::{Add, Shl, Shr, Sub};

pub trait ShiftRightCeil: Copy {
    fn shrc<T: Copy>(self, rhs: T) -> Self
    where
        Self: Shr<T, Output = Self> + Shl<T, Output = Self>;
}

impl<S: Copy + Add<Self, Output = Self> + Sub<Self, Output = Self> + From<u8>> ShiftRightCeil
    for S
{
    #[inline(always)]
    fn shrc<T: Copy>(self, rhs: T) -> Self
    where
        Self: Shr<T, Output = Self> + Shl<T, Output = Self>,
    {
        (self + (Self::from(1u8) << rhs) - Self::from(1u8)) >> rhs
    }
}
