/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::cmp::min;

/// An Equation\<W\> is a representation of a GF(2) linear functional
///     a(x) = b + sum_i a_i x_i
/// where a_i is equal to zero except for i in a block of 64*W coefficients
/// starting at i=s. We say an Equation is /aligned/ if a_s = 1.
/// (Note: a_i above denotes the i-th bit, not the i'th 64-bit limb.)
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Equation<const W: usize> {
    pub s: usize,    
    pub a: [u64; W], 
    pub b: u8,       
}

impl<const W: usize> Equation<W> {
    /// Construct the equation a(x) = sum_{i=s}^{s+64*W} a_i x^i.
    /// The result is aligned.
    pub fn homogeneous(s: usize, a: [u64; W]) -> Equation<W> {
        Equation::inhomogeneous(s, a, 0)
    }

    /// Construct the equation a(x) = b + sum_{i=s}^{s+64*W} a_i x^i.
    /// The result is aligned.
    pub fn inhomogeneous(s: usize, a: [u64; W], b: u8) -> Equation<W> {
        let mut eq = Equation { s: 0, a, b };
        eq.add(&Equation::zero());
        eq.s += s;
        eq
    }

    /// Construct the equation a(x) = 0.
    pub fn zero() -> Self {
        Equation {
            s: 0,
            a: [0u64; W],
            b: 0,
        }
    }

    /// Is this a(x) = 1 or a(x) = 0?
    pub fn is_zero(&self) -> bool {
        self.a == [0u64; W]
    }

    /// Adds `other` into `self`, i.e. sets self.a ^= other.a and self.b ^= other.b and then aligns
    /// the result.
    pub fn add(&mut self, other: &Equation<W>) {
        assert!(self.s == other.s);
        for i in 0..W {
            self.a[i] ^= other.a[i];
        }
        self.b ^= other.b;
        if self.is_zero() {
            return;
        }
        while self.a[0] == 0 {
            self.a.rotate_left(1);
        }
        let k = self.a[0].trailing_zeros();
        if k == 0 {
            return;
        }
        for i in 0..W - 1 {
            self.a[i] >>= k;
            self.a[i] |= self.a[i + 1] << (64 - k);
        }
        self.a[W - 1] >>= k;
        self.s += k as usize;
    }

    /// Computes a(z) = sum a_i z_i.
    pub fn eval(&self, z: &[u64]) -> u8 {
        let limb = self.s / 64;
        let shift = self.s % 64;
        let mut r = 0;
        for i in limb..min(z.len(), limb + W) {
            let mut tmp = z[i] >> shift;
            if i + 1 < z.len() && shift != 0 {
                tmp |= z[i + 1] << (64 - shift);
            }
            r ^= tmp & self.a[i - limb];
        }
        (r.count_ones() & 1) as u8
    }
}
