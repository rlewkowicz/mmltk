//! An iterator over bitmasks.

/// An iterator that produces the set bits in the given `u64`.
///
/// `OneBitsIter(n)` is an [`Iterator`] that produces each of the set bits in
/// `n`, as a bitmask, in order of increasing value. In other words, it produces
/// the unique sequence of distinct powers of two that adds up to `n`.
///
/// For example, iterating over `OneBitsIter(21)` produces the values `1`, `4`,
/// and `16`, in that order, because `21` is `0xb10101`.
///
/// When `n` is the bits of a bitmask, this iterates over the set bits in the
/// bitmask, in order of increasing bit value. `bitflags` does define an `iter`
/// method, but it's not well-specified or well-implemented.
///
/// The values produced are masks, not bit numbers. Use `u64::trailing_zeros` if
/// you need bit numbers.
pub struct OneBitsIter(u64);

impl OneBitsIter {
    pub const fn new(bits: u64) -> Self {
        Self(bits)
    }
}

impl Iterator for OneBitsIter {
    type Item = u64;

    fn next(&mut self) -> Option<Self::Item> {
        if self.0 == 0 {
            return None;
        }

        let mask = self.0 - 1;

        let item = self.0 & !mask;

        self.0 &= mask;

        Some(item)
    }
}
