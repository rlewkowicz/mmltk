// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.
// The C++ implementation preserved here in comments is licensed as follows:
// Copyright (C) 2015 THL A29 Limited, a Tencent company, and Milo Yip. All
// Licensed under the MIT License (the "License"); you may not use this file
// except in compliance with the License. You may obtain a copy of the License
// http://opensource.org/licenses/MIT
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// License for the specific language governing permissions and limitations under
// the License.

use std::ops;

#[derive(Copy, Clone, Debug)]
pub struct DiyFp<F, E> {
    pub f: F,
    pub e: E,
}

impl<F, E> DiyFp<F, E> {
    pub fn new(f: F, e: E) -> Self {
        DiyFp { f: f, e: e }
    }
}

impl ops::Mul for DiyFp<u32, i32> {
    type Output = Self;
    fn mul(self, rhs: Self) -> Self {
        let mut tmp = self.f as u64 * rhs.f as u64;
        tmp += 1u64 << 31; 
        DiyFp {
            f: (tmp >> 32) as u32,
            e: self.e + rhs.e + 32,
        }
    }
}

impl ops::Mul for DiyFp<u64, isize> {
    type Output = Self;
    fn mul(self, rhs: Self) -> Self {
        let m32 = 0xFFFFFFFFu64;
        let a = self.f >> 32;
        let b = self.f & m32;
        let c = rhs.f >> 32;
        let d = rhs.f & m32;
        let ac = a * c;
        let bc = b * c;
        let ad = a * d;
        let bd = b * d;
        let mut tmp = (bd >> 32) + (ad & m32) + (bc & m32);
        tmp += 1u64 << 31; 
        DiyFp {
            f: ac + (ad >> 32) + (bc >> 32) + (tmp >> 32),
            e: self.e + rhs.e + 64,
        }
    }
}

#[doc(hidden)]
#[macro_export]
macro_rules! diyfp {(
    floating_type: $fty:ty,
    significand_type: $sigty:ty,
    exponent_type: $expty:ty,

    diy_significand_size: $diy_significand_size:expr,
    significand_size: $significand_size:expr,
    exponent_bias: $exponent_bias:expr,
    mask_type: $mask_type:ty,
    exponent_mask: $exponent_mask:expr,
    significand_mask: $significand_mask:expr,
    hidden_bit: $hidden_bit:expr,
    cached_powers_f: $cached_powers_f:expr,
    cached_powers_e: $cached_powers_e:expr,
    min_power: $min_power:expr,
) => {

type DiyFp = diyfp::DiyFp<$sigty, $expty>;

impl DiyFp {
    unsafe fn from(d: $fty) -> Self {
        let u: $mask_type = mem::transmute(d);

        let biased_e = ((u & $exponent_mask) >> $significand_size) as $expty;
        let significand = u & $significand_mask;
        if biased_e != 0 {
            DiyFp {
                f: significand + $hidden_bit,
                e: biased_e - $exponent_bias - $significand_size,
            }
        } else {
            DiyFp {
                f: significand,
                e: 1 - $exponent_bias - $significand_size,
            }
        }
    }

    fn normalize(self) -> DiyFp {
        let mut res = self;
        while (res.f & (1 << ($diy_significand_size - 1))) == 0 {
            res.f <<= 1;
            res.e -= 1;
        }
        res
    }

    fn normalize_boundary(self) -> DiyFp {
        let mut res = self;
        while (res.f & $hidden_bit << 1) == 0 {
            res.f <<= 1;
            res.e -= 1;
        }
        res.f <<= $diy_significand_size - $significand_size - 2;
        res.e -= $diy_significand_size - $significand_size - 2;
        res
    }

    fn normalized_boundaries(self) -> (DiyFp, DiyFp) {
        let pl = DiyFp::new((self.f << 1) + 1, self.e - 1).normalize_boundary();
        let mut mi = if self.f == $hidden_bit {
            DiyFp::new((self.f << 2) - 1, self.e - 2)
        } else {
            DiyFp::new((self.f << 1) - 1, self.e - 1)
        };
        mi.f <<= mi.e - pl.e;
        mi.e = pl.e;
        (mi, pl)
    }
}

impl ops::Sub for DiyFp {
    type Output = Self;
    fn sub(self, rhs: Self) -> Self {
        DiyFp {
            f: self.f - rhs.f,
            e: self.e,
        }
    }
}

#[inline]
fn get_cached_power(e: $expty) -> (DiyFp, isize) {
    let dk = (3 - $diy_significand_size - e) as f64 * 0.30102999566398114f64 - ($min_power + 1) as f64;
    let mut k = dk as isize;
    if dk - k as f64 > 0.0 {
        k += 1;
    }

    let index = ((k >> 3) + 1) as usize;
    let k = -($min_power + (index << 3) as isize);

    (DiyFp::new($cached_powers_f[index], $cached_powers_e[index] as $expty), k)
}

}}
