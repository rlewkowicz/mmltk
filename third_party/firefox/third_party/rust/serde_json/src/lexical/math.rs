
//! Building-blocks for arbitrary-precision math.
//!
//! These algorithms assume little-endian order for the large integer
//! buffers, so for a `vec![0, 1, 2, 3]`, `3` is the most significant limb,
//! and `0` is the least significant limb.

use super::large_powers;
use super::num::*;
use super::small_powers::*;
use alloc::vec::Vec;
use core::{cmp, iter, mem};



#[cfg(fast_arithmetic = "32")]
pub type Limb = u32;

#[cfg(fast_arithmetic = "32")]
pub const POW5_LIMB: &[Limb] = &POW5_32;

#[cfg(fast_arithmetic = "32")]
pub const POW10_LIMB: &[Limb] = &POW10_32;

#[cfg(fast_arithmetic = "32")]
type Wide = u64;

#[cfg(fast_arithmetic = "64")]
pub type Limb = u64;

#[cfg(fast_arithmetic = "64")]
pub const POW5_LIMB: &[Limb] = &POW5_64;

#[cfg(fast_arithmetic = "64")]
pub const POW10_LIMB: &[Limb] = &POW10_64;

#[cfg(fast_arithmetic = "64")]
type Wide = u128;

/// Cast to limb type.
#[inline]
pub(crate) fn as_limb<T: Integer>(t: T) -> Limb {
    Limb::as_cast(t)
}

/// Cast to wide type.
#[inline]
fn as_wide<T: Integer>(t: T) -> Wide {
    Wide::as_cast(t)
}


/// Split u64 into limbs, in little-endian order.
#[inline]
#[cfg(fast_arithmetic = "32")]
fn split_u64(x: u64) -> [Limb; 2] {
    [as_limb(x), as_limb(x >> 32)]
}

/// Split u64 into limbs, in little-endian order.
#[inline]
#[cfg(fast_arithmetic = "64")]
fn split_u64(x: u64) -> [Limb; 1] {
    [as_limb(x)]
}



/// Check if any of the remaining bits are non-zero.
#[inline]
pub fn nonzero<T: Integer>(x: &[T], rindex: usize) -> bool {
    let len = x.len();
    let slc = &x[..len - rindex];
    slc.iter().rev().any(|&x| x != T::ZERO)
}

/// Shift 64-bit integer to high 64-bits.
#[inline]
fn u64_to_hi64_1(r0: u64) -> (u64, bool) {
    debug_assert!(r0 != 0);
    let ls = r0.leading_zeros();
    (r0 << ls, false)
}

/// Shift 2 64-bit integers to high 64-bits.
#[inline]
fn u64_to_hi64_2(r0: u64, r1: u64) -> (u64, bool) {
    debug_assert!(r0 != 0);
    let ls = r0.leading_zeros();
    let rs = 64 - ls;
    let v = match ls {
        0 => r0,
        _ => (r0 << ls) | (r1 >> rs),
    };
    let n = r1 << ls != 0;
    (v, n)
}

/// Trait to export the high 64-bits from a little-endian slice.
trait Hi64<T>: AsRef<[T]> {
    /// Get the hi64 bits from a 1-limb slice.
    fn hi64_1(&self) -> (u64, bool);

    /// Get the hi64 bits from a 2-limb slice.
    fn hi64_2(&self) -> (u64, bool);

    /// Get the hi64 bits from a 3-limb slice.
    fn hi64_3(&self) -> (u64, bool);

    /// High-level exporter to extract the high 64 bits from a little-endian slice.
    #[inline]
    fn hi64(&self) -> (u64, bool) {
        match self.as_ref().len() {
            0 => (0, false),
            1 => self.hi64_1(),
            2 => self.hi64_2(),
            _ => self.hi64_3(),
        }
    }
}

impl Hi64<u32> for [u32] {
    #[inline]
    fn hi64_1(&self) -> (u64, bool) {
        debug_assert!(self.len() == 1);
        let r0 = self[0] as u64;
        u64_to_hi64_1(r0)
    }

    #[inline]
    fn hi64_2(&self) -> (u64, bool) {
        debug_assert!(self.len() == 2);
        let r0 = (self[1] as u64) << 32;
        let r1 = self[0] as u64;
        u64_to_hi64_1(r0 | r1)
    }

    #[inline]
    fn hi64_3(&self) -> (u64, bool) {
        debug_assert!(self.len() >= 3);
        let r0 = self[self.len() - 1] as u64;
        let r1 = (self[self.len() - 2] as u64) << 32;
        let r2 = self[self.len() - 3] as u64;
        let (v, n) = u64_to_hi64_2(r0, r1 | r2);
        (v, n || nonzero(self, 3))
    }
}

impl Hi64<u64> for [u64] {
    #[inline]
    fn hi64_1(&self) -> (u64, bool) {
        debug_assert!(self.len() == 1);
        let r0 = self[0];
        u64_to_hi64_1(r0)
    }

    #[inline]
    fn hi64_2(&self) -> (u64, bool) {
        debug_assert!(self.len() >= 2);
        let r0 = self[self.len() - 1];
        let r1 = self[self.len() - 2];
        let (v, n) = u64_to_hi64_2(r0, r1);
        (v, n || nonzero(self, 2))
    }

    #[inline]
    fn hi64_3(&self) -> (u64, bool) {
        self.hi64_2()
    }
}



mod scalar {
    use super::*;


    /// Add two small integers and return the resulting value and if overflow happens.
    #[inline]
    pub fn add(x: Limb, y: Limb) -> (Limb, bool) {
        x.overflowing_add(y)
    }

    /// AddAssign two small integers and return if overflow happens.
    #[inline]
    pub fn iadd(x: &mut Limb, y: Limb) -> bool {
        let t = add(*x, y);
        *x = t.0;
        t.1
    }


    /// Subtract two small integers and return the resulting value and if overflow happens.
    #[inline]
    pub fn sub(x: Limb, y: Limb) -> (Limb, bool) {
        x.overflowing_sub(y)
    }

    /// SubAssign two small integers and return if overflow happens.
    #[inline]
    pub fn isub(x: &mut Limb, y: Limb) -> bool {
        let t = sub(*x, y);
        *x = t.0;
        t.1
    }


    /// Multiply two small integers (with carry) (and return the overflow contribution).
    ///
    /// Returns the (low, high) components.
    #[inline]
    pub fn mul(x: Limb, y: Limb, carry: Limb) -> (Limb, Limb) {
        let z: Wide = as_wide(x) * as_wide(y) + as_wide(carry);
        let bits = mem::size_of::<Limb>() * 8;
        (as_limb(z), as_limb(z >> bits))
    }

    /// Multiply two small integers (with carry) (and return if overflow happens).
    #[inline]
    pub fn imul(x: &mut Limb, y: Limb, carry: Limb) -> Limb {
        let t = mul(*x, y, carry);
        *x = t.0;
        t.1
    }
} 



mod small {
    use super::*;


    /// Implied AddAssign implementation for adding a small integer to bigint.
    ///
    /// Allows us to choose a start-index in x to store, to allow incrementing
    /// from a non-zero start.
    #[inline]
    pub fn iadd_impl(x: &mut Vec<Limb>, y: Limb, xstart: usize) {
        if x.len() <= xstart {
            x.push(y);
        } else {
            let mut carry = scalar::iadd(&mut x[xstart], y);

            let mut size = xstart + 1;
            while carry && size < x.len() {
                carry = scalar::iadd(&mut x[size], 1);
                size += 1;
            }

            if carry {
                x.push(1);
            }
        }
    }

    /// AddAssign small integer to bigint.
    #[inline]
    pub fn iadd(x: &mut Vec<Limb>, y: Limb) {
        iadd_impl(x, y, 0);
    }


    /// SubAssign small integer to bigint.
    /// Does not do overflowing subtraction.
    #[inline]
    pub fn isub_impl(x: &mut Vec<Limb>, y: Limb, xstart: usize) {
        debug_assert!(x.len() > xstart && (x[xstart] >= y || x.len() > xstart + 1));

        let mut carry = scalar::isub(&mut x[xstart], y);

        let mut size = xstart + 1;
        while carry && size < x.len() {
            carry = scalar::isub(&mut x[size], 1);
            size += 1;
        }
        normalize(x);
    }


    /// MulAssign small integer to bigint.
    #[inline]
    pub fn imul(x: &mut Vec<Limb>, y: Limb) {
        let mut carry: Limb = 0;
        for xi in &mut *x {
            carry = scalar::imul(xi, y, carry);
        }

        if carry != 0 {
            x.push(carry);
        }
    }

    /// Mul small integer to bigint.
    #[inline]
    pub fn mul(x: &[Limb], y: Limb) -> Vec<Limb> {
        let mut z = Vec::<Limb>::default();
        z.extend_from_slice(x);
        imul(&mut z, y);
        z
    }

    /// MulAssign by a power.
    ///
    /// Theoretically...
    ///
    /// Use an exponentiation by squaring method, since it reduces the time
    /// complexity of the multiplication to ~`O(log(n))` for the squaring,
    /// and `O(n*m)` for the result. Since `m` is typically a lower-order
    /// factor, this significantly reduces the number of multiplications
    /// we need to do. Iteratively multiplying by small powers follows
    /// the nth triangular number series, which scales as `O(p^2)`, but
    /// where `p` is `n+m`. In short, it scales very poorly.
    ///
    /// Practically....
    ///
    /// Exponentiation by Squaring:
    ///     running 2 tests
    ///     test bigcomp_f32_lexical ... bench:       1,018 ns/iter (+/- 78)
    ///     test bigcomp_f64_lexical ... bench:       3,639 ns/iter (+/- 1,007)
    ///
    /// Exponentiation by Iterative Small Powers:
    ///     running 2 tests
    ///     test bigcomp_f32_lexical ... bench:         518 ns/iter (+/- 31)
    ///     test bigcomp_f64_lexical ... bench:         583 ns/iter (+/- 47)
    ///
    /// Exponentiation by Iterative Large Powers (of 2):
    ///     running 2 tests
    ///     test bigcomp_f32_lexical ... bench:         671 ns/iter (+/- 31)
    ///     test bigcomp_f64_lexical ... bench:       1,394 ns/iter (+/- 47)
    ///
    /// Even using worst-case scenarios, exponentiation by squaring is
    /// significantly slower for our workloads. Just multiply by small powers,
    /// in simple cases, and use precalculated large powers in other cases.
    pub fn imul_pow5(x: &mut Vec<Limb>, n: u32) {
        use super::large::KARATSUBA_CUTOFF;

        let small_powers = POW5_LIMB;
        let large_powers = large_powers::POW5;

        if n == 0 {
            return;
        }

        let bit_length = 32 - n.leading_zeros() as usize;
        debug_assert!(bit_length != 0 && bit_length <= large_powers.len());
        if x.len() + large_powers[bit_length - 1].len() < 2 * KARATSUBA_CUTOFF {

            let step = small_powers.len() - 1;
            let power = small_powers[step];
            let mut n = n as usize;
            while n >= step {
                imul(x, power);
                n -= step;
            }

            imul(x, small_powers[n]);
        } else {

            let mut idx: usize = 0;
            let mut bit: usize = 1;
            let mut n = n as usize;
            while n != 0 {
                if n & bit != 0 {
                    debug_assert!(idx < large_powers.len());
                    large::imul(x, large_powers[idx]);
                    n ^= bit;
                }
                idx += 1;
                bit <<= 1;
            }
        }
    }


    /// Get number of leading zero bits in the storage.
    #[inline]
    pub fn leading_zeros(x: &[Limb]) -> usize {
        x.last().map_or(0, |x| x.leading_zeros() as usize)
    }

    /// Calculate the bit-length of the big-integer.
    #[inline]
    pub fn bit_length(x: &[Limb]) -> usize {
        let bits = mem::size_of::<Limb>() * 8;
        let nlz = leading_zeros(x);
        bits.checked_mul(x.len())
            .map_or_else(usize::max_value, |v| v - nlz)
    }


    /// Shift-left bits inside a buffer.
    ///
    /// Assumes `n < Limb::BITS`, IE, internally shifting bits.
    #[inline]
    pub fn ishl_bits(x: &mut Vec<Limb>, n: usize) {
        let bits = mem::size_of::<Limb>() * 8;
        debug_assert!(n < bits);
        if n == 0 {
            return;
        }

        let rshift = bits - n;
        let lshift = n;
        let mut prev: Limb = 0;
        for xi in &mut *x {
            let tmp = *xi;
            *xi <<= lshift;
            *xi |= prev >> rshift;
            prev = tmp;
        }

        let carry = prev >> rshift;
        if carry != 0 {
            x.push(carry);
        }
    }

    /// Shift-left `n` digits inside a buffer.
    ///
    /// Assumes `n` is not 0.
    #[inline]
    pub fn ishl_limbs(x: &mut Vec<Limb>, n: usize) {
        debug_assert!(n != 0);
        if !x.is_empty() {
            x.reserve(n);
            x.splice(..0, iter::repeat(0).take(n));
        }
    }

    /// Shift-left buffer by n bits.
    #[inline]
    pub fn ishl(x: &mut Vec<Limb>, n: usize) {
        let bits = mem::size_of::<Limb>() * 8;
        let rem = n % bits;
        let div = n / bits;
        ishl_bits(x, rem);
        if div != 0 {
            ishl_limbs(x, div);
        }
    }


    /// Normalize the container by popping any leading zeros.
    #[inline]
    pub fn normalize(x: &mut Vec<Limb>) {
        while x.last() == Some(&0) {
            x.pop();
        }
    }
} 



mod large {
    use super::*;


    /// Compare `x` to `y`, in little-endian order.
    #[inline]
    pub fn compare(x: &[Limb], y: &[Limb]) -> cmp::Ordering {
        if x.len() > y.len() {
            cmp::Ordering::Greater
        } else if x.len() < y.len() {
            cmp::Ordering::Less
        } else {
            let iter = x.iter().rev().zip(y.iter().rev());
            for (&xi, &yi) in iter {
                if xi > yi {
                    return cmp::Ordering::Greater;
                } else if xi < yi {
                    return cmp::Ordering::Less;
                }
            }
            cmp::Ordering::Equal
        }
    }

    /// Check if x is less than y.
    #[inline]
    pub fn less(x: &[Limb], y: &[Limb]) -> bool {
        compare(x, y) == cmp::Ordering::Less
    }

    /// Check if x is greater than or equal to y.
    #[inline]
    pub fn greater_equal(x: &[Limb], y: &[Limb]) -> bool {
        !less(x, y)
    }


    /// Implied AddAssign implementation for bigints.
    ///
    /// Allows us to choose a start-index in x to store, so we can avoid
    /// padding the buffer with zeros when not needed, optimized for vectors.
    pub fn iadd_impl(x: &mut Vec<Limb>, y: &[Limb], xstart: usize) {
        if y.len() > x.len() - xstart {
            x.resize(y.len() + xstart, 0);
        }

        let mut carry = false;
        for (xi, yi) in x[xstart..].iter_mut().zip(y.iter()) {
            let mut tmp = scalar::iadd(xi, *yi);
            if carry {
                tmp |= scalar::iadd(xi, 1);
            }
            carry = tmp;
        }

        if carry {
            small::iadd_impl(x, 1, y.len() + xstart);
        }
    }

    /// AddAssign bigint to bigint.
    #[inline]
    pub fn iadd(x: &mut Vec<Limb>, y: &[Limb]) {
        iadd_impl(x, y, 0);
    }

    /// Add bigint to bigint.
    #[inline]
    pub fn add(x: &[Limb], y: &[Limb]) -> Vec<Limb> {
        let mut z = Vec::<Limb>::default();
        z.extend_from_slice(x);
        iadd(&mut z, y);
        z
    }


    /// SubAssign bigint to bigint.
    pub fn isub(x: &mut Vec<Limb>, y: &[Limb]) {
        debug_assert!(greater_equal(x, y));

        let mut carry = false;
        for (xi, yi) in x.iter_mut().zip(y.iter()) {
            let mut tmp = scalar::isub(xi, *yi);
            if carry {
                tmp |= scalar::isub(xi, 1);
            }
            carry = tmp;
        }

        if carry {
            small::isub_impl(x, 1, y.len());
        } else {
            small::normalize(x);
        }
    }


    /// Number of digits to bottom-out to asymptotically slow algorithms.
    ///
    /// Karatsuba tends to out-perform long-multiplication at ~320-640 bits,
    /// so we go halfway, while Newton division tends to out-perform
    /// Algorithm D at ~1024 bits. We can toggle this for optimal performance.
    pub const KARATSUBA_CUTOFF: usize = 32;

    /// Grade-school multiplication algorithm.
    ///
    /// Slow, naive algorithm, using limb-bit bases and just shifting left for
    /// each iteration. This could be optimized with numerous other algorithms,
    /// but it's extremely simple, and works in O(n*m) time, which is fine
    /// by me. Each iteration, of which there are `m` iterations, requires
    /// `n` multiplications, and `n` additions, or grade-school multiplication.
    fn long_mul(x: &[Limb], y: &[Limb]) -> Vec<Limb> {
        let mut z: Vec<Limb> = small::mul(x, y[0]);
        z.resize(x.len() + y.len(), 0);

        for (i, &yi) in y[1..].iter().enumerate() {
            let zi: Vec<Limb> = small::mul(x, yi);
            iadd_impl(&mut z, &zi, i + 1);
        }

        small::normalize(&mut z);

        z
    }

    /// Split two buffers into halfway, into (lo, hi).
    #[inline]
    pub fn karatsuba_split(z: &[Limb], m: usize) -> (&[Limb], &[Limb]) {
        (&z[..m], &z[m..])
    }

    /// Karatsuba multiplication algorithm with roughly equal input sizes.
    ///
    /// Assumes `y.len() >= x.len()`.
    fn karatsuba_mul(x: &[Limb], y: &[Limb]) -> Vec<Limb> {
        if y.len() <= KARATSUBA_CUTOFF {
            long_mul(x, y)
        } else if x.len() < y.len() / 2 {
            karatsuba_uneven_mul(x, y)
        } else {
            let m = y.len() / 2;
            let (xl, xh) = karatsuba_split(x, m);
            let (yl, yh) = karatsuba_split(y, m);
            let sumx = add(xl, xh);
            let sumy = add(yl, yh);
            let z0 = karatsuba_mul(xl, yl);
            let mut z1 = karatsuba_mul(&sumx, &sumy);
            let z2 = karatsuba_mul(xh, yh);
            isub(&mut z1, &z2);
            isub(&mut z1, &z0);

            let len = z0.len().max(m + z1.len()).max(2 * m + z2.len());
            let mut result = z0;
            result.reserve_exact(len - result.len());
            iadd_impl(&mut result, &z1, m);
            iadd_impl(&mut result, &z2, 2 * m);

            result
        }
    }

    /// Karatsuba multiplication algorithm where y is substantially larger than x.
    ///
    /// Assumes `y.len() >= x.len()`.
    fn karatsuba_uneven_mul(x: &[Limb], mut y: &[Limb]) -> Vec<Limb> {
        let mut result = Vec::<Limb>::default();
        result.resize(x.len() + y.len(), 0);

        let mut start = 0;
        while !y.is_empty() {
            let m = x.len().min(y.len());
            let (yl, yh) = karatsuba_split(y, m);
            let prod = karatsuba_mul(x, yl);
            iadd_impl(&mut result, &prod, start);
            y = yh;
            start += m;
        }
        small::normalize(&mut result);

        result
    }

    /// Forwarder to the proper Karatsuba algorithm.
    #[inline]
    fn karatsuba_mul_fwd(x: &[Limb], y: &[Limb]) -> Vec<Limb> {
        if x.len() < y.len() {
            karatsuba_mul(x, y)
        } else {
            karatsuba_mul(y, x)
        }
    }

    /// MulAssign bigint to bigint.
    #[inline]
    pub fn imul(x: &mut Vec<Limb>, y: &[Limb]) {
        if y.len() == 1 {
            small::imul(x, y[0]);
        } else {
            *x = karatsuba_mul_fwd(x, y);
        }
    }
} 


/// Traits for shared operations for big integers.
///
/// None of these are implemented using normal traits, since these
/// are very expensive operations, and we want to deliberately
/// and explicitly use these functions.
pub(crate) trait Math: Clone + Sized + Default {

    /// Get access to the underlying data
    fn data(&self) -> &Vec<Limb>;

    /// Get access to the underlying data
    fn data_mut(&mut self) -> &mut Vec<Limb>;


    /// Compare self to y.
    #[inline]
    fn compare(&self, y: &Self) -> cmp::Ordering {
        large::compare(self.data(), y.data())
    }


    /// Get the high 64-bits from the bigint and if there are remaining bits.
    #[inline]
    fn hi64(&self) -> (u64, bool) {
        self.data().as_slice().hi64()
    }

    /// Calculate the bit-length of the big-integer.
    /// Returns usize::max_value() if the value overflows,
    /// IE, if `self.data().len() > usize::max_value() / 8`.
    #[inline]
    fn bit_length(&self) -> usize {
        small::bit_length(self.data())
    }


    /// Create new big integer from u64.
    #[inline]
    fn from_u64(x: u64) -> Self {
        let mut v = Self::default();
        let slc = split_u64(x);
        v.data_mut().extend_from_slice(&slc);
        v.normalize();
        v
    }


    /// Normalize the integer, so any leading zero values are removed.
    #[inline]
    fn normalize(&mut self) {
        small::normalize(self.data_mut());
    }


    /// AddAssign small integer.
    #[inline]
    fn iadd_small(&mut self, y: Limb) {
        small::iadd(self.data_mut(), y);
    }


    /// MulAssign small integer.
    #[inline]
    fn imul_small(&mut self, y: Limb) {
        small::imul(self.data_mut(), y);
    }

    /// Multiply by a power of 2.
    #[inline]
    fn imul_pow2(&mut self, n: u32) {
        self.ishl(n as usize);
    }

    /// Multiply by a power of 5.
    #[inline]
    fn imul_pow5(&mut self, n: u32) {
        small::imul_pow5(self.data_mut(), n);
    }

    /// MulAssign by a power of 10.
    #[inline]
    fn imul_pow10(&mut self, n: u32) {
        self.imul_pow5(n);
        self.imul_pow2(n);
    }


    /// Shift-left the entire buffer n bits.
    #[inline]
    fn ishl(&mut self, n: usize) {
        small::ishl(self.data_mut(), n);
    }
}
