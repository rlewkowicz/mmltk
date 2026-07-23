//! A simple big-integer type for slow path algorithms.
//!
//! This includes minimal stackvector for use in big-integer arithmetic.

#![doc(hidden)]

#[cfg(feature = "alloc")]
use crate::heapvec::HeapVec;
use crate::num::Float;
#[cfg(not(feature = "alloc"))]
use crate::stackvec::StackVec;
#[cfg(not(feature = "compact"))]
use crate::table::{LARGE_POW5, LARGE_POW5_STEP};
use core::{cmp, ops, ptr};

/// Number of bits in a Bigint.
///
/// This needs to be at least the number of bits required to store
/// a Bigint, which is `log2(radix**digits)`.
/// ≅ 3600 for base-10, rounded-up.
pub const BIGINT_BITS: usize = 4000;

/// The number of limbs for the bigint.
pub const BIGINT_LIMBS: usize = BIGINT_BITS / LIMB_BITS;

#[cfg(feature = "alloc")]
pub type VecType = HeapVec;

#[cfg(not(feature = "alloc"))]
pub type VecType = StackVec;

/// Storage for a big integer type.
///
/// This is used for algorithms when we have a finite number of digits.
/// Specifically, it stores all the significant digits scaled to the
/// proper exponent, as an integral type, and then directly compares
/// these digits.
///
/// This requires us to store the number of significant bits, plus the
/// number of exponent bits (required) since we scale everything
/// to the same exponent.
#[derive(Clone, PartialEq, Eq)]
pub struct Bigint {
    /// Significant digits for the float, stored in a big integer in LE order.
    ///
    /// This is pretty much the same number of digits for any radix, since the
    ///  significant digits balances out the zeros from the exponent:
    ///     1. Decimal is 1091 digits, 767 mantissa digits + 324 exponent zeros.
    ///     2. Base 6 is 1097 digits, or 680 mantissa digits + 417 exponent zeros.
    ///     3. Base 36 is 1086 digits, or 877 mantissa digits + 209 exponent zeros.
    ///
    /// However, the number of bytes required is larger for large radixes:
    /// for decimal, we need `log2(10**1091) ≅ 3600`, while for base 36
    /// we need `log2(36**1086) ≅ 5600`. Since we use uninitialized data,
    /// we avoid a major performance hit from the large buffer size.
    pub data: VecType,
}

#[allow(clippy::new_without_default)]
impl Bigint {
    /// Construct a bigint representing 0.
    #[inline(always)]
    pub fn new() -> Self {
        Self {
            data: VecType::new(),
        }
    }

    /// Construct a bigint from an integer.
    #[inline(always)]
    pub fn from_u64(value: u64) -> Self {
        Self {
            data: VecType::from_u64(value),
        }
    }

    #[inline(always)]
    pub fn hi64(&self) -> (u64, bool) {
        self.data.hi64()
    }

    /// Multiply and assign as if by exponentiation by a power.
    #[inline]
    pub fn pow(&mut self, base: u32, exp: u32) -> Option<()> {
        debug_assert!(base == 2 || base == 5 || base == 10);
        if base % 5 == 0 {
            pow(&mut self.data, exp)?;
        }
        if base % 2 == 0 {
            shl(&mut self.data, exp as usize)?;
        }
        Some(())
    }

    /// Calculate the bit-length of the big-integer.
    #[inline]
    pub fn bit_length(&self) -> u32 {
        bit_length(&self.data)
    }
}

impl ops::MulAssign<&Bigint> for Bigint {
    fn mul_assign(&mut self, rhs: &Bigint) {
        self.data *= &rhs.data;
    }
}

/// REVERSE VIEW

/// Reverse, immutable view of a sequence.
pub struct ReverseView<'a, T: 'a> {
    inner: &'a [T],
}

impl<'a, T> ops::Index<usize> for ReverseView<'a, T> {
    type Output = T;

    #[inline]
    fn index(&self, index: usize) -> &T {
        let len = self.inner.len();
        &(*self.inner)[len - index - 1]
    }
}

/// Create a reverse view of the vector for indexing.
#[inline]
pub fn rview(x: &[Limb]) -> ReverseView<Limb> {
    ReverseView {
        inner: x,
    }
}


/// Compare `x` to `y`, in little-endian order.
#[inline]
pub fn compare(x: &[Limb], y: &[Limb]) -> cmp::Ordering {
    match x.len().cmp(&y.len()) {
        cmp::Ordering::Equal => {
            let iter = x.iter().rev().zip(y.iter().rev());
            for (&xi, yi) in iter {
                match xi.cmp(yi) {
                    cmp::Ordering::Equal => (),
                    ord => return ord,
                }
            }
            cmp::Ordering::Equal
        },
        ord => ord,
    }
}


/// Normalize the integer, so any leading zero values are removed.
#[inline]
pub fn normalize(x: &mut VecType) {
    while let Some(&value) = x.get(x.len().wrapping_sub(1)) {
        if value == 0 {
            unsafe { x.set_len(x.len() - 1) };
        } else {
            break;
        }
    }
}

/// Get if the big integer is normalized.
#[inline]
#[allow(clippy::match_like_matches_macro)]
pub fn is_normalized(x: &[Limb]) -> bool {
    match x.get(x.len().wrapping_sub(1)) {
        Some(&0) => false,
        _ => true,
    }
}


/// Create StackVec from u64 value.
#[inline(always)]
#[allow(clippy::branches_sharing_code)]
pub fn from_u64(x: u64) -> VecType {
    let mut vec = VecType::new();
    debug_assert!(vec.capacity() >= 2);
    if LIMB_BITS == 32 {
        vec.try_push(x as Limb).unwrap();
        vec.try_push((x >> 32) as Limb).unwrap();
    } else {
        vec.try_push(x as Limb).unwrap();
    }
    vec.normalize();
    vec
}


/// Check if any of the remaining bits are non-zero.
///
/// # Safety
///
/// Safe as long as `rindex <= x.len()`.
#[inline]
pub fn nonzero(x: &[Limb], rindex: usize) -> bool {
    debug_assert!(rindex <= x.len());

    let len = x.len();
    let slc = &x[..len - rindex];
    slc.iter().rev().any(|&x| x != 0)
}


/// Shift 32-bit integer to high 64-bits.
#[inline]
pub fn u32_to_hi64_1(r0: u32) -> (u64, bool) {
    u64_to_hi64_1(r0 as u64)
}

/// Shift 2 32-bit integers to high 64-bits.
#[inline]
pub fn u32_to_hi64_2(r0: u32, r1: u32) -> (u64, bool) {
    let r0 = (r0 as u64) << 32;
    let r1 = r1 as u64;
    u64_to_hi64_1(r0 | r1)
}

/// Shift 3 32-bit integers to high 64-bits.
#[inline]
pub fn u32_to_hi64_3(r0: u32, r1: u32, r2: u32) -> (u64, bool) {
    let r0 = r0 as u64;
    let r1 = (r1 as u64) << 32;
    let r2 = r2 as u64;
    u64_to_hi64_2(r0, r1 | r2)
}

/// Shift 64-bit integer to high 64-bits.
#[inline]
pub fn u64_to_hi64_1(r0: u64) -> (u64, bool) {
    let ls = r0.leading_zeros();
    (r0 << ls, false)
}

/// Shift 2 64-bit integers to high 64-bits.
#[inline]
pub fn u64_to_hi64_2(r0: u64, r1: u64) -> (u64, bool) {
    let ls = r0.leading_zeros();
    let rs = 64 - ls;
    let v = match ls {
        0 => r0,
        _ => (r0 << ls) | (r1 >> rs),
    };
    let n = r1 << ls != 0;
    (v, n)
}

/// Extract the hi bits from the buffer.
macro_rules! hi {
    (@1 $self:ident, $rview:ident, $t:ident, $fn:ident) => {{
        $fn($rview[0] as $t)
    }};

    (@2 $self:ident, $rview:ident, $t:ident, $fn:ident) => {{
        let r0 = $rview[0] as $t;
        let r1 = $rview[1] as $t;
        $fn(r0, r1)
    }};

    (@nonzero2 $self:ident, $rview:ident, $t:ident, $fn:ident) => {{
        let (v, n) = hi!(@2 $self, $rview, $t, $fn);
        (v, n || nonzero($self, 2 ))
    }};

    (@3 $self:ident, $rview:ident, $t:ident, $fn:ident) => {{
        let r0 = $rview[0] as $t;
        let r1 = $rview[1] as $t;
        let r2 = $rview[2] as $t;
        $fn(r0, r1, r2)
    }};

    (@nonzero3 $self:ident, $rview:ident, $t:ident, $fn:ident) => {{
        let (v, n) = hi!(@3 $self, $rview, $t, $fn);
        (v, n || nonzero($self, 3))
    }};
}

/// Get the high 64 bits from the vector.
#[inline(always)]
pub fn hi64(x: &[Limb]) -> (u64, bool) {
    let rslc = rview(x);
    match x.len() {
        0 => (0, false),
        1 if LIMB_BITS == 32 => hi!(@1 x, rslc, u32, u32_to_hi64_1),
        1 => hi!(@1 x, rslc, u64, u64_to_hi64_1),
        2 if LIMB_BITS == 32 => hi!(@2 x, rslc, u32, u32_to_hi64_2),
        2 => hi!(@2 x, rslc, u64, u64_to_hi64_2),
        _ if LIMB_BITS == 32 => hi!(@nonzero3 x, rslc, u32, u32_to_hi64_3),
        _ => hi!(@nonzero2 x, rslc, u64, u64_to_hi64_2),
    }
}


/// MulAssign by a power of 5.
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
/// The following benchmarks were run on `1 * 5^300`, using native `pow`,
/// a version with only small powers, and one with pre-computed powers
/// of `5^(3 * max_exp)`, rather than `5^(5 * max_exp)`.
///
/// However, using large powers is crucial for good performance for higher
/// powers.
///     pow/default             time:   [426.20 ns 427.96 ns 429.89 ns]
///     pow/small               time:   [2.9270 us 2.9411 us 2.9565 us]
///     pow/large:3             time:   [838.51 ns 842.21 ns 846.27 ns]
///
/// Even using worst-case scenarios, exponentiation by squaring is
/// significantly slower for our workloads. Just multiply by small powers,
/// in simple cases, and use precalculated large powers in other cases.
///
/// Furthermore, using sufficiently big large powers is also crucial for
/// performance. This is a tradeoff of binary size and performance, and
/// using a single value at ~`5^(5 * max_exp)` seems optimal.
pub fn pow(x: &mut VecType, mut exp: u32) -> Option<()> {
    #[cfg(not(feature = "compact"))]
    {
        while exp >= LARGE_POW5_STEP {
            large_mul(x, &LARGE_POW5)?;
            exp -= LARGE_POW5_STEP;
        }
    }

    let small_step = if LIMB_BITS == 32 {
        13
    } else {
        27
    };
    let max_native = (5 as Limb).pow(small_step);
    while exp >= small_step {
        small_mul(x, max_native)?;
        exp -= small_step;
    }
    if exp != 0 {
        let small_power = unsafe { f64::int_pow_fast_path(exp as usize, 5) };
        small_mul(x, small_power as Limb)?;
    }
    Some(())
}


/// Add two small integers and return the resulting value and if overflow happens.
#[inline(always)]
pub fn scalar_add(x: Limb, y: Limb) -> (Limb, bool) {
    x.overflowing_add(y)
}

/// Multiply two small integers (with carry) (and return the overflow contribution).
///
/// Returns the (low, high) components.
#[inline(always)]
pub fn scalar_mul(x: Limb, y: Limb, carry: Limb) -> (Limb, Limb) {
    let z: Wide = (x as Wide) * (y as Wide) + (carry as Wide);
    (z as Limb, (z >> LIMB_BITS) as Limb)
}


/// Add small integer to bigint starting from offset.
#[inline]
pub fn small_add_from(x: &mut VecType, y: Limb, start: usize) -> Option<()> {
    let mut index = start;
    let mut carry = y;
    while carry != 0 && index < x.len() {
        let result = scalar_add(x[index], carry);
        x[index] = result.0;
        carry = result.1 as Limb;
        index += 1;
    }
    if carry != 0 {
        x.try_push(carry)?;
    }
    Some(())
}

/// Add small integer to bigint.
#[inline(always)]
pub fn small_add(x: &mut VecType, y: Limb) -> Option<()> {
    small_add_from(x, y, 0)
}

/// Multiply bigint by small integer.
#[inline]
pub fn small_mul(x: &mut VecType, y: Limb) -> Option<()> {
    let mut carry = 0;
    for xi in x.iter_mut() {
        let result = scalar_mul(*xi, y, carry);
        *xi = result.0;
        carry = result.1;
    }
    if carry != 0 {
        x.try_push(carry)?;
    }
    Some(())
}


/// Add bigint to bigint starting from offset.
pub fn large_add_from(x: &mut VecType, y: &[Limb], start: usize) -> Option<()> {
    if y.len() > x.len().saturating_sub(start) {
        x.try_resize(y.len() + start, 0)?;
    }

    let mut carry = false;
    for (index, &yi) in y.iter().enumerate() {
        let xi = x.get_mut(start + index).unwrap();

        let result = scalar_add(*xi, yi);
        *xi = result.0;
        let mut tmp = result.1;
        if carry {
            let result = scalar_add(*xi, 1);
            *xi = result.0;
            tmp |= result.1;
        }
        carry = tmp;
    }

    if carry {
        small_add_from(x, 1, y.len() + start)?;
    }
    Some(())
}

/// Add bigint to bigint.
#[inline(always)]
pub fn large_add(x: &mut VecType, y: &[Limb]) -> Option<()> {
    large_add_from(x, y, 0)
}

/// Grade-school multiplication algorithm.
///
/// Slow, naive algorithm, using limb-bit bases and just shifting left for
/// each iteration. This could be optimized with numerous other algorithms,
/// but it's extremely simple, and works in O(n*m) time, which is fine
/// by me. Each iteration, of which there are `m` iterations, requires
/// `n` multiplications, and `n` additions, or grade-school multiplication.
///
/// Don't use Karatsuba multiplication, since out implementation seems to
/// be slower asymptotically, which is likely just due to the small sizes
/// we deal with here. For example, running on the following data:
///
/// ```text
/// const SMALL_X: &[u32] = &[
///     766857581, 3588187092, 1583923090, 2204542082, 1564708913, 2695310100, 3676050286,
///     1022770393, 468044626, 446028186
/// ];
/// const SMALL_Y: &[u32] = &[
///     3945492125, 3250752032, 1282554898, 1708742809, 1131807209, 3171663979, 1353276095,
///     1678845844, 2373924447, 3640713171
/// ];
/// const LARGE_X: &[u32] = &[
///     3647536243, 2836434412, 2154401029, 1297917894, 137240595, 790694805, 2260404854,
///     3872698172, 690585094, 99641546, 3510774932, 1672049983, 2313458559, 2017623719,
///     638180197, 1140936565, 1787190494, 1797420655, 14113450, 2350476485, 3052941684,
///     1993594787, 2901001571, 4156930025, 1248016552, 848099908, 2660577483, 4030871206,
///     692169593, 2835966319, 1781364505, 4266390061, 1813581655, 4210899844, 2137005290,
///     2346701569, 3715571980, 3386325356, 1251725092, 2267270902, 474686922, 2712200426,
///     197581715, 3087636290, 1379224439, 1258285015, 3230794403, 2759309199, 1494932094,
///     326310242
/// ];
/// const LARGE_Y: &[u32] = &[
///     1574249566, 868970575, 76716509, 3198027972, 1541766986, 1095120699, 3891610505,
///     2322545818, 1677345138, 865101357, 2650232883, 2831881215, 3985005565, 2294283760,
///     3468161605, 393539559, 3665153349, 1494067812, 106699483, 2596454134, 797235106,
///     705031740, 1209732933, 2732145769, 4122429072, 141002534, 790195010, 4014829800,
///     1303930792, 3649568494, 308065964, 1233648836, 2807326116, 79326486, 1262500691,
///     621809229, 2258109428, 3819258501, 171115668, 1139491184, 2979680603, 1333372297,
///     1657496603, 2790845317, 4090236532, 4220374789, 601876604, 1828177209, 2372228171,
///     2247372529
/// ];
/// ```
///
/// We get the following results:

/// ```text
/// mul/small:long          time:   [220.23 ns 221.47 ns 222.81 ns]
/// Found 4 outliers among 100 measurements (4.00%)
///   2 (2.00%) high mild
///   2 (2.00%) high severe
/// mul/small:karatsuba     time:   [233.88 ns 234.63 ns 235.44 ns]
/// Found 11 outliers among 100 measurements (11.00%)
///   8 (8.00%) high mild
///   3 (3.00%) high severe
/// mul/large:long          time:   [1.9365 us 1.9455 us 1.9558 us]
/// Found 12 outliers among 100 measurements (12.00%)
///   7 (7.00%) high mild
///   5 (5.00%) high severe
/// mul/large:karatsuba     time:   [4.4250 us 4.4515 us 4.4812 us]
/// ```
///
/// In short, Karatsuba multiplication is never worthwhile for out use-case.
pub fn long_mul(x: &[Limb], y: &[Limb]) -> Option<VecType> {
    let mut z = VecType::try_from(x)?;
    if !y.is_empty() {
        let y0 = y[0];
        small_mul(&mut z, y0)?;

        for (index, &yi) in y.iter().enumerate().skip(1) {
            if yi != 0 {
                let mut zi = VecType::try_from(x)?;
                small_mul(&mut zi, yi)?;
                large_add_from(&mut z, &zi, index)?;
            }
        }
    }

    z.normalize();
    Some(z)
}

/// Multiply bigint by bigint using grade-school multiplication algorithm.
#[inline(always)]
pub fn large_mul(x: &mut VecType, y: &[Limb]) -> Option<()> {
    if y.len() == 1 {
        small_mul(x, y[0])?;
    } else {
        *x = long_mul(y, x)?;
    }
    Some(())
}


/// Shift-left `n` bits inside a buffer.
#[inline]
pub fn shl_bits(x: &mut VecType, n: usize) -> Option<()> {
    debug_assert!(n != 0);

    debug_assert!(n < LIMB_BITS);
    let rshift = LIMB_BITS - n;
    let lshift = n;
    let mut prev: Limb = 0;
    for xi in x.iter_mut() {
        let tmp = *xi;
        *xi <<= lshift;
        *xi |= prev >> rshift;
        prev = tmp;
    }

    let carry = prev >> rshift;
    if carry != 0 {
        x.try_push(carry)?;
    }

    Some(())
}

/// Shift-left `n` limbs inside a buffer.
#[inline]
pub fn shl_limbs(x: &mut VecType, n: usize) -> Option<()> {
    debug_assert!(n != 0);
    if n + x.len() > x.capacity() {
        None
    } else if !x.is_empty() {
        let len = n + x.len();
        unsafe {
            let src = x.as_ptr();
            let dst = x.as_mut_ptr().add(n);
            ptr::copy(src, dst, x.len());
            ptr::write_bytes(x.as_mut_ptr(), 0, n);
            x.set_len(len);
        }
        Some(())
    } else {
        Some(())
    }
}

/// Shift-left buffer by n bits.
#[inline]
pub fn shl(x: &mut VecType, n: usize) -> Option<()> {
    let rem = n % LIMB_BITS;
    let div = n / LIMB_BITS;
    if rem != 0 {
        shl_bits(x, rem)?;
    }
    if div != 0 {
        shl_limbs(x, div)?;
    }
    Some(())
}

/// Get number of leading zero bits in the storage.
#[inline]
pub fn leading_zeros(x: &[Limb]) -> u32 {
    let length = x.len();
    if let Some(&value) = x.get(length.wrapping_sub(1)) {
        value.leading_zeros()
    } else {
        0
    }
}

/// Calculate the bit-length of the big-integer.
#[inline]
pub fn bit_length(x: &[Limb]) -> u32 {
    let nlz = leading_zeros(x);
    LIMB_BITS as u32 * x.len() as u32 - nlz
}



#[cfg(all(target_pointer_width = "64", not(target_arch = "sparc")))]
pub type Limb = u64;
#[cfg(all(target_pointer_width = "64", not(target_arch = "sparc")))]
pub type Wide = u128;
#[cfg(all(target_pointer_width = "64", not(target_arch = "sparc")))]
pub const LIMB_BITS: usize = 64;

#[cfg(not(all(target_pointer_width = "64", not(target_arch = "sparc"))))]
pub type Limb = u32;
#[cfg(not(all(target_pointer_width = "64", not(target_arch = "sparc"))))]
pub type Wide = u64;
#[cfg(not(all(target_pointer_width = "64", not(target_arch = "sparc"))))]
pub const LIMB_BITS: usize = 32;
