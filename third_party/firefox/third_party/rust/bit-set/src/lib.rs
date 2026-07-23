// Copyright 2012-2014 The Rust Project Developers. See the COPYRIGHT
// file at the top-level directory of this distribution and at
// http://rust-lang.org/COPYRIGHT.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! # Description
//!
//! An implementation of a set using a bit vector as an underlying
//! representation for holding unsigned numerical elements.
//!
//! It should also be noted that the amount of storage necessary for holding a
//! set of objects is proportional to the maximum of the objects when viewed
//! as a `usize`.
//!
//! # Examples
//!
//! ```
//! use bit_set::BitSet;
//!
//! // It's a regular set
//! let mut s = BitSet::new();
//! s.insert(0);
//! s.insert(3);
//! s.insert(7);
//!
//! s.remove(7);
//!
//! if !s.contains(7) {
//!     println!("There is no 7");
//! }
//!
//! // Can initialize from a `BitVec`
//! let other = BitSet::from_bytes(&[0b11010000]);
//!
//! s.union_with(&other);
//!
//! // Print 0, 1, 3 in some order
//! for x in s.iter() {
//!     println!("{}", x);
//! }
//!
//! // Can convert back to a `BitVec`
//! let bv = s.into_bit_vec();
//! assert!(bv[3]);
//! ```
#![doc(html_root_url = "https://docs.rs/bit-set/0.10.0/bit_set/")]
#![deny(clippy::shadow_reuse)]
#![deny(clippy::shadow_same)]
#![deny(clippy::shadow_unrelated)]
#![no_std]

#[cfg(feature = "std")]
extern crate std;

pub use bit_vec::BitBlock;

use bit_vec::{BitVec, Blocks};
use core::cmp;
use core::cmp::Ordering;
use core::fmt;
use core::hash;
use core::iter::{self, Chain, Enumerate, FromIterator, Repeat, Skip, Take};

#[cfg(feature = "nanoserde")]
extern crate alloc;
#[cfg(feature = "nanoserde")]
use alloc::vec::Vec;
#[cfg(feature = "nanoserde")]
use nanoserde::{DeBin, DeJson, DeRon, SerBin, SerJson, SerRon};

type MatchWords<'a, B> = Chain<Enumerate<Blocks<'a, B>>, Skip<Take<Enumerate<Repeat<B>>>>>;

/// Computes how many blocks are needed to store that many bits
fn blocks_for_bits<B: BitBlock>(bits: usize) -> usize {
    if bits % B::bits() == 0 {
        bits / B::bits()
    } else {
        bits / B::bits() + 1
    }
}

#[allow(clippy::iter_skip_zero)]
fn match_words<'a, 'b, B: BitBlock>(
    a: &'a BitVec<B>,
    b: &'b BitVec<B>,
) -> (MatchWords<'a, B>, MatchWords<'b, B>) {
    let a_len = a.storage().len();
    let b_len = b.storage().len();

    if a_len < b_len {
        (
            a.blocks()
                .enumerate()
                .chain(iter::repeat(B::zero()).enumerate().take(b_len).skip(a_len)),
            b.blocks()
                .enumerate()
                .chain(iter::repeat(B::zero()).enumerate().take(0).skip(0)),
        )
    } else {
        (
            a.blocks()
                .enumerate()
                .chain(iter::repeat(B::zero()).enumerate().take(0).skip(0)),
            b.blocks()
                .enumerate()
                .chain(iter::repeat(B::zero()).enumerate().take(a_len).skip(b_len)),
        )
    }
}

#[cfg_attr(feature = "serde", derive(serde::Deserialize, serde::Serialize))]
#[cfg_attr(feature = "borsh", derive(borsh::BorshDeserialize, borsh::BorshSerialize))]
#[cfg_attr(feature = "miniserde", derive(miniserde::Deserialize, miniserde::Serialize))]
#[cfg_attr(feature = "nanoserde", derive(DeBin, DeJson, DeRon, SerBin, SerJson, SerRon))]
pub struct BitSet<B = u32> {
    bit_vec: BitVec<B>,
}

impl<B: BitBlock> Clone for BitSet<B> {
    fn clone(&self) -> Self {
        BitSet {
            bit_vec: self.bit_vec.clone(),
        }
    }

    fn clone_from(&mut self, other: &Self) {
        self.bit_vec.clone_from(&other.bit_vec);
    }
}

impl<B: BitBlock> Default for BitSet<B> {
    #[inline]
    fn default() -> Self {
        BitSet {
            bit_vec: Default::default(),
        }
    }
}

impl<B: BitBlock> FromIterator<usize> for BitSet<B> {
    fn from_iter<I: IntoIterator<Item = usize>>(iter: I) -> Self {
        let mut ret = Self::default();
        ret.extend(iter);
        ret
    }
}

impl<B: BitBlock> Extend<usize> for BitSet<B> {
    #[inline]
    fn extend<I: IntoIterator<Item = usize>>(&mut self, iter: I) {
        for i in iter {
            self.insert(i);
        }
    }
}

impl<B: BitBlock> PartialOrd for BitSet<B> {
    #[inline]
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl<B: BitBlock> Ord for BitSet<B> {
    #[inline]
    fn cmp(&self, other: &Self) -> Ordering {
        self.iter().cmp(other)
    }
}

impl<B: BitBlock> PartialEq for BitSet<B> {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        self.iter().eq(other)
    }
}

impl<B: BitBlock> Eq for BitSet<B> {}

impl BitSet<u32> {
    /// Creates a new empty `BitSet`.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut s = BitSet::new();
    /// ```
    #[inline]
    pub fn new() -> Self {
        Self::default()
    }

    /// Creates a new `BitSet` with initially no contents, able to
    /// hold `nbits` elements without resizing.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut s = BitSet::with_capacity(100);
    /// assert!(s.capacity() >= 100);
    /// ```
    #[inline]
    pub fn with_capacity(nbits: usize) -> Self {
        let bit_vec = BitVec::from_elem(nbits, false);
        Self::from_bit_vec(bit_vec)
    }

    /// Creates a new `BitSet` from the given bit vector.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_vec::BitVec;
    /// use bit_set::BitSet;
    ///
    /// let bv = BitVec::from_bytes(&[0b01100000]);
    /// let s = BitSet::from_bit_vec(bv);
    ///
    /// // Print 1, 2 in arbitrary order
    /// for x in s.iter() {
    ///     println!("{}", x);
    /// }
    /// ```
    #[inline]
    pub fn from_bit_vec(bit_vec: BitVec) -> Self {
        BitSet { bit_vec }
    }

    pub fn from_bytes(bytes: &[u8]) -> Self {
        BitSet {
            bit_vec: BitVec::from_bytes(bytes),
        }
    }
}

impl<B: BitBlock> BitSet<B> {
    /// Creates a new empty `BitSet`.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut s = <BitSet>::new_general();
    /// ```
    #[inline]
    pub fn new_general() -> Self {
        Self::default()
    }

    /// Creates a new `BitSet` with initially no contents, able to
    /// hold `nbits` elements without resizing.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut s = <BitSet>::with_capacity_general(100);
    /// assert!(s.capacity() >= 100);
    /// ```
    #[inline]
    pub fn with_capacity_general(nbits: usize) -> Self {
        let bit_vec = BitVec::from_elem_general(nbits, false);
        Self::from_bit_vec_general(bit_vec)
    }

    /// Creates a new `BitSet` from the given bit vector.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_vec::BitVec;
    /// use bit_set::BitSet;
    ///
    /// let bv: BitVec<u64> = BitVec::from_bytes_general(&[0b01100000]);
    /// let s = BitSet::from_bit_vec_general(bv);
    ///
    /// // Print 1, 2 in arbitrary order
    /// for x in s.iter() {
    ///     println!("{}", x);
    /// }
    /// ```
    #[inline]
    pub fn from_bit_vec_general(bit_vec: BitVec<B>) -> Self {
        BitSet { bit_vec }
    }

    pub fn from_bytes_general(bytes: &[u8]) -> Self {
        BitSet {
            bit_vec: BitVec::from_bytes_general(bytes),
        }
    }

    /// Returns the capacity in bits for this bit vector. Inserting any
    /// element less than this amount will not trigger a resizing.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut s = BitSet::with_capacity(100);
    /// assert!(s.capacity() >= 100);
    /// ```
    #[inline]
    pub fn capacity(&self) -> usize {
        self.bit_vec.capacity()
    }

    /// Reserves capacity for the given `BitSet` to contain `len` distinct elements. In the case
    /// of `BitSet` this means reallocations will not occur as long as all inserted elements
    /// are less than `len`.
    ///
    /// The collection may reserve more space to avoid frequent reallocations.
    ///
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut s = BitSet::new();
    /// s.reserve_len(10);
    /// assert!(s.capacity() >= 10);
    /// ```
    pub fn reserve_len(&mut self, len: usize) {
        let cur_len = self.bit_vec.len();
        if len >= cur_len {
            self.bit_vec.reserve(len - cur_len);
        }
    }

    /// Reserves the minimum capacity for the given `BitSet` to contain `len` distinct elements.
    /// In the case of `BitSet` this means reallocations will not occur as long as all inserted
    /// elements are less than `len`.
    ///
    /// Note that the allocator may give the collection more space than it requests. Therefore
    /// capacity can not be relied upon to be precisely minimal. Prefer `reserve_len` if future
    /// insertions are expected.
    ///
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut s = BitSet::new();
    /// s.reserve_len_exact(10);
    /// assert!(s.capacity() >= 10);
    /// ```
    pub fn reserve_len_exact(&mut self, len: usize) {
        let cur_len = self.bit_vec.len();
        if len >= cur_len {
            self.bit_vec.reserve_exact(len - cur_len);
        }
    }

    /// Consumes this set to return the underlying bit vector.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut s = BitSet::new();
    /// s.insert(0);
    /// s.insert(3);
    ///
    /// let bv = s.into_bit_vec();
    /// assert!(bv[0]);
    /// assert!(bv[3]);
    /// ```
    #[inline]
    pub fn into_bit_vec(self) -> BitVec<B> {
        self.bit_vec
    }

    /// Returns a reference to the underlying bit vector.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut set = BitSet::new();
    /// set.insert(0);
    ///
    /// let bv = set.get_ref();
    /// assert_eq!(bv[0], true);
    /// ```
    #[inline]
    pub fn get_ref(&self) -> &BitVec<B> {
        &self.bit_vec
    }

    /// Returns a mutable reference to the underlying bit vector.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut set = BitSet::new();
    /// set.insert(0);
    /// set.insert(3);
    ///
    /// {
    ///     let bv = set.get_mut();
    ///     bv.set(1, true);
    /// }
    ///
    /// assert!(set.contains(0));
    /// assert!(set.contains(1));
    /// assert!(set.contains(3));
    /// ```
    #[inline]
    pub fn get_mut(&mut self) -> &mut BitVec<B> {
        &mut self.bit_vec
    }

    #[inline]
    fn other_op<F>(&mut self, other: &Self, mut f: F)
    where
        F: FnMut(B, B) -> B,
    {
        let self_bit_vec = &mut self.bit_vec;
        let other_bit_vec = &other.bit_vec;

        let self_len = self_bit_vec.len();
        let other_len = other_bit_vec.len();

        if self_len < other_len {
            self_bit_vec.grow(other_len - self_len, false);
        }

        let other_words = {
            let (_, result) = match_words(self_bit_vec, other_bit_vec);
            result
        };

        for (i, w) in other_words {
            let old = self_bit_vec.storage()[i];
            let new = f(old, w);
            unsafe {
                self_bit_vec.storage_mut()[i] = new;
            }
        }
    }

    /// Truncates the underlying vector to the least length required.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let mut s = BitSet::new();
    /// s.insert(3231);
    /// s.remove(3231);
    ///
    /// // Internal storage will probably be bigger than necessary
    /// println!("old capacity: {}", s.capacity());
    /// assert!(s.capacity() >= 3231);
    ///
    /// // Now should be smaller
    /// s.shrink_to_fit();
    /// println!("new capacity: {}", s.capacity());
    /// ```
    #[inline]
    pub fn shrink_to_fit(&mut self) {
        let bit_vec = &mut self.bit_vec;
        let old_len = bit_vec.storage().len();
        let n = bit_vec
            .storage()
            .iter()
            .rev()
            .take_while(|&&n| n == B::zero())
            .count();
        let trunc_len = old_len - n;
        unsafe {
            bit_vec.storage_mut().truncate(trunc_len);
            bit_vec.set_len(trunc_len * B::bits());
        }
        bit_vec.shrink_to_fit();
    }

    /// Iterator over each usize stored in the `BitSet`.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let s = BitSet::from_bytes(&[0b01001010]);
    ///
    /// // Print 1, 4, 6 in arbitrary order
    /// for x in s.iter() {
    ///     println!("{}", x);
    /// }
    /// ```
    #[inline]
    pub fn iter(&self) -> Iter<'_, B> {
        Iter(BlockIter::from_blocks(self.bit_vec.blocks()))
    }

    /// Iterator over each usize stored in `self` union `other`.
    /// See [`union_with`] for an efficient in-place version.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let a = BitSet::from_bytes(&[0b01101000]);
    /// let b = BitSet::from_bytes(&[0b10100000]);
    ///
    /// // Print 0, 1, 2, 4 in arbitrary order
    /// for x in a.union(&b) {
    ///     println!("{}", x);
    /// }
    /// ```
    ///
    /// [`union_with`]: Self::union_with
    #[inline]
    pub fn union<'a>(&'a self, other: &'a Self) -> Union<'a, B> {
        fn or<B: BitBlock>(w1: B, w2: B) -> B {
            w1 | w2
        }

        Union(BlockIter::from_blocks(TwoBitPositions {
            set: self.bit_vec.blocks(),
            other: other.bit_vec.blocks(),
            merge: or,
        }))
    }

    /// Iterator over each usize stored in `self` intersect `other`.
    /// See [`intersect_with`] for an efficient in-place version.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let a = BitSet::from_bytes(&[0b01101000]);
    /// let b = BitSet::from_bytes(&[0b10100000]);
    ///
    /// // Print 2
    /// for x in a.intersection(&b) {
    ///     println!("{}", x);
    /// }
    /// ```
    ///
    /// [`intersect_with`]: Self::intersect_with
    #[inline]
    pub fn intersection<'a>(&'a self, other: &'a Self) -> Intersection<'a, B> {
        fn bitand<B: BitBlock>(w1: B, w2: B) -> B {
            w1 & w2
        }
        let min = cmp::min(self.bit_vec.len(), other.bit_vec.len());

        Intersection {
            iter: BlockIter::from_blocks(TwoBitPositions {
                set: self.bit_vec.blocks(),
                other: other.bit_vec.blocks(),
                merge: bitand,
            }),
            n: min,
        }
    }

    /// Iterator over each usize stored in the `self` setminus `other`.
    /// See [`difference_with`] for an efficient in-place version.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let a = BitSet::from_bytes(&[0b01101000]);
    /// let b = BitSet::from_bytes(&[0b10100000]);
    ///
    /// // Print 1, 4 in arbitrary order
    /// for x in a.difference(&b) {
    ///     println!("{}", x);
    /// }
    ///
    /// // Note that difference is not symmetric,
    /// // and `b - a` means something else.
    /// // This prints 0
    /// for x in b.difference(&a) {
    ///     println!("{}", x);
    /// }
    /// ```
    ///
    /// [`difference_with`]: Self::difference_with
    #[inline]
    pub fn difference<'a>(&'a self, other: &'a Self) -> Difference<'a, B> {
        fn diff<B: BitBlock>(w1: B, w2: B) -> B {
            w1 & !w2
        }

        Difference(BlockIter::from_blocks(TwoBitPositions {
            set: self.bit_vec.blocks(),
            other: other.bit_vec.blocks(),
            merge: diff,
        }))
    }

    /// Iterator over each usize stored in the symmetric difference of `self` and `other`.
    /// See [`symmetric_difference_with`] for an efficient in-place version.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let a = BitSet::from_bytes(&[0b01101000]);
    /// let b = BitSet::from_bytes(&[0b10100000]);
    ///
    /// // Print 0, 1, 4 in arbitrary order
    /// for x in a.symmetric_difference(&b) {
    ///     println!("{}", x);
    /// }
    /// ```
    ///
    /// [`symmetric_difference_with`]: Self::symmetric_difference_with
    #[inline]
    pub fn symmetric_difference<'a>(&'a self, other: &'a Self) -> SymmetricDifference<'a, B> {
        fn bitxor<B: BitBlock>(w1: B, w2: B) -> B {
            w1 ^ w2
        }

        SymmetricDifference(BlockIter::from_blocks(TwoBitPositions {
            set: self.bit_vec.blocks(),
            other: other.bit_vec.blocks(),
            merge: bitxor,
        }))
    }

    /// Unions in-place with the specified other bit vector.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let a   = 0b01101000;
    /// let b   = 0b10100000;
    /// let res = 0b11101000;
    ///
    /// let mut a = BitSet::from_bytes(&[a]);
    /// let b = BitSet::from_bytes(&[b]);
    /// let res = BitSet::from_bytes(&[res]);
    ///
    /// a.union_with(&b);
    /// assert_eq!(a, res);
    /// ```
    #[inline]
    pub fn union_with(&mut self, other: &Self) {
        self.other_op(other, |w1, w2| w1 | w2);
    }

    /// Intersects in-place with the specified other bit vector.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let a   = 0b01101000;
    /// let b   = 0b10100000;
    /// let res = 0b00100000;
    ///
    /// let mut a = BitSet::from_bytes(&[a]);
    /// let b = BitSet::from_bytes(&[b]);
    /// let res = BitSet::from_bytes(&[res]);
    ///
    /// a.intersect_with(&b);
    /// assert_eq!(a, res);
    /// ```
    #[inline]
    pub fn intersect_with(&mut self, other: &Self) {
        self.other_op(other, |w1, w2| w1 & w2);
    }

    /// Makes this bit vector the difference with the specified other bit vector
    /// in-place.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let a   = 0b01101000;
    /// let b   = 0b10100000;
    /// let a_b = 0b01001000; // a - b
    /// let b_a = 0b10000000; // b - a
    ///
    /// let mut bva = BitSet::from_bytes(&[a]);
    /// let bvb = BitSet::from_bytes(&[b]);
    /// let bva_b = BitSet::from_bytes(&[a_b]);
    /// let bvb_a = BitSet::from_bytes(&[b_a]);
    ///
    /// bva.difference_with(&bvb);
    /// assert_eq!(bva, bva_b);
    ///
    /// let bva = BitSet::from_bytes(&[a]);
    /// let mut bvb = BitSet::from_bytes(&[b]);
    ///
    /// bvb.difference_with(&bva);
    /// assert_eq!(bvb, bvb_a);
    /// ```
    #[inline]
    pub fn difference_with(&mut self, other: &Self) {
        self.other_op(other, |w1, w2| w1 & !w2);
    }

    /// Makes this bit vector the symmetric difference with the specified other
    /// bit vector in-place.
    ///
    /// # Examples
    ///
    /// ```
    /// use bit_set::BitSet;
    ///
    /// let a   = 0b01101000;
    /// let b   = 0b10100000;
    /// let res = 0b11001000;
    ///
    /// let mut a = BitSet::from_bytes(&[a]);
    /// let b = BitSet::from_bytes(&[b]);
    /// let res = BitSet::from_bytes(&[res]);
    ///
    /// a.symmetric_difference_with(&b);
    /// assert_eq!(a, res);
    /// ```
    #[inline]
    pub fn symmetric_difference_with(&mut self, other: &Self) {
        self.other_op(other, |w1, w2| w1 ^ w2);
    }


    /// Counts the number of set bits in this set.
    ///
    /// Note that this function scans the set to calculate the number.
    #[inline]
    pub fn count(&self) -> usize {
        self.bit_vec.blocks().fold(0, |acc, n| acc + n.count_ones())
    }

    /// Counts the number of set bits in this set.
    ///
    /// Note that this function scans the set to calculate the number.
    #[inline]
    #[deprecated = "use BitSet::count() instead"]
    pub fn len(&self) -> usize {
        self.count()
    }

    /// Returns whether there are no bits set in this set
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.bit_vec.none()
    }

    /// Removes all elements of this set.
    ///
    /// Different from [`reset`] only in that the capacity is preserved.
    ///
    /// [`reset`]: Self::reset
    #[inline]
    pub fn make_empty(&mut self) {
        self.bit_vec.fill(false);
    }

    /// Resets this set to an empty state.
    ///
    /// Different from [`make_empty`] only in that the capacity may NOT be preserved.
    ///
    /// [`make_empty`]: Self::make_empty
    #[inline]
    pub fn reset(&mut self) {
        self.bit_vec.remove_all();
    }

    /// Clears all bits in this set
    #[deprecated(since = "0.9.0", note = "please use `fn make_empty` instead")]
    #[inline]
    pub fn clear(&mut self) {
        self.make_empty();
    }

    /// Returns `true` if this set contains the specified integer.
    #[inline]
    pub fn contains(&self, value: usize) -> bool {
        let bit_vec = &self.bit_vec;
        value < bit_vec.len() && bit_vec[value]
    }

    /// Returns `true` if the set has no elements in common with `other`.
    /// This is equivalent to checking for an empty intersection.
    #[inline]
    pub fn is_disjoint(&self, other: &Self) -> bool {
        self.intersection(other).next().is_none()
    }

    /// Returns `true` if the set is a subset of another.
    #[inline]
    pub fn is_subset(&self, other: &Self) -> bool {
        let self_bit_vec = &self.bit_vec;
        let other_bit_vec = &other.bit_vec;
        let other_blocks = blocks_for_bits::<B>(other_bit_vec.len());

        self_bit_vec.blocks().zip(other_bit_vec.blocks()).all(|(w1, w2)| w1 & w2 == w1) &&
        self_bit_vec.blocks().skip(other_blocks).all(|w| w == B::zero())
    }

    /// Returns `true` if the set is a superset of another.
    #[inline]
    pub fn is_superset(&self, other: &Self) -> bool {
        other.is_subset(self)
    }

    /// Adds a value to the set. Returns `true` if the value was not already
    /// present in the set.
    pub fn insert(&mut self, value: usize) -> bool {
        if self.contains(value) {
            return false;
        }

        let len = self.bit_vec.len();
        if value >= len {
            self.bit_vec.grow(value - len + 1, false);
        }

        self.bit_vec.set(value, true);
        true
    }

    /// Removes a value from the set. Returns `true` if the value was
    /// present in the set.
    pub fn remove(&mut self, value: usize) -> bool {
        if !self.contains(value) {
            return false;
        }

        self.bit_vec.set(value, false);

        true
    }

    /// Excludes `element` and all greater elements from the `BitSet`.
    pub fn truncate(&mut self, element: usize) {
        self.bit_vec.truncate(element);
    }
}

impl<B: BitBlock> fmt::Debug for BitSet<B> {
    fn fmt(&self, fmt: &mut fmt::Formatter) -> fmt::Result {
        fmt.debug_struct("BitSet")
            .field("bit_vec", &self.bit_vec)
            .finish()
    }
}

impl<B: BitBlock> fmt::Display for BitSet<B> {
    fn fmt(&self, fmt: &mut fmt::Formatter) -> fmt::Result {
        fmt.debug_set().entries(self).finish()
    }
}

impl<B: BitBlock> hash::Hash for BitSet<B> {
    fn hash<H: hash::Hasher>(&self, state: &mut H) {
        for pos in self {
            pos.hash(state);
        }
    }
}

#[derive(Clone)]
struct BlockIter<T, B> {
    head: B,
    head_offset: usize,
    tail: T,
}

impl<T, B: BitBlock> BlockIter<T, B>
where
    T: Iterator<Item = B>,
{
    fn from_blocks(mut blocks: T) -> BlockIter<T, B> {
        let h = blocks.next().unwrap_or_else(B::zero);
        BlockIter {
            tail: blocks,
            head: h,
            head_offset: 0,
        }
    }
}

/// An iterator combining two `BitSet` iterators.
#[derive(Clone)]
struct TwoBitPositions<'a, B: 'a> {
    set: Blocks<'a, B>,
    other: Blocks<'a, B>,
    merge: fn(B, B) -> B,
}

/// An iterator for `BitSet`.
#[derive(Clone)]
pub struct Iter<'a, B: 'a>(BlockIter<Blocks<'a, B>, B>);
#[derive(Clone)]
pub struct Union<'a, B: 'a>(BlockIter<TwoBitPositions<'a, B>, B>);
#[derive(Clone)]
pub struct Intersection<'a, B: 'a> {
    iter: BlockIter<TwoBitPositions<'a, B>, B>,
    n: usize,
}
#[derive(Clone)]
pub struct Difference<'a, B: 'a>(BlockIter<TwoBitPositions<'a, B>, B>);
#[derive(Clone)]
pub struct SymmetricDifference<'a, B: 'a>(BlockIter<TwoBitPositions<'a, B>, B>);

impl<T, B: BitBlock> Iterator for BlockIter<T, B>
where
    T: Iterator<Item = B>,
{
    type Item = usize;

    fn next(&mut self) -> Option<usize> {
        while self.head == B::zero() {
            match self.tail.next() {
                Some(w) => self.head = w,
                None => return None,
            }
            self.head_offset += B::bits();
        }

        let k = (self.head & (!self.head + B::one())) - B::one();
        self.head = self.head & (self.head - B::one());
        Some(self.head_offset + (B::count_ones(k)))
    }

    fn count(self) -> usize {
        self.head.count_ones() + self.tail.map(|block| block.count_ones()).sum::<usize>()
    }

    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        match self.tail.size_hint() {
            (_, Some(h)) => (0, Some((1 + h) * B::bits())),
            _ => (0, None),
        }
    }
}

impl<B: BitBlock> Iterator for TwoBitPositions<'_, B> {
    type Item = B;

    fn next(&mut self) -> Option<B> {
        match (self.set.next(), self.other.next()) {
            (Some(a), Some(b)) => Some((self.merge)(a, b)),
            (Some(a), None) => Some((self.merge)(a, B::zero())),
            (None, Some(b)) => Some((self.merge)(B::zero(), b)),
            _ => None,
        }
    }

    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        let (first_lower_bound, first_upper_bound) = self.set.size_hint();
        let (second_lower_bound, second_upper_bound) = self.other.size_hint();

        let upper_bound = first_upper_bound.zip(second_upper_bound);

        let get_max = |(a, b)| cmp::max(a, b);
        (
            cmp::max(first_lower_bound, second_lower_bound),
            upper_bound.map(get_max),
        )
    }
}

impl<B: BitBlock> Iterator for Iter<'_, B> {
    type Item = usize;

    #[inline]
    fn next(&mut self) -> Option<usize> {
        self.0.next()
    }
    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        self.0.size_hint()
    }
    #[inline]
    fn count(self) -> usize {
        self.0.count()
    }
}

impl<B: BitBlock> Iterator for Union<'_, B> {
    type Item = usize;

    #[inline]
    fn next(&mut self) -> Option<usize> {
        self.0.next()
    }
    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        self.0.size_hint()
    }
    #[inline]
    fn count(self) -> usize {
        self.0.count()
    }
}

impl<B: BitBlock> Iterator for Intersection<'_, B> {
    type Item = usize;

    #[inline]
    fn next(&mut self) -> Option<usize> {
        if self.n != 0 {
            self.n -= 1;
            self.iter.next()
        } else {
            None
        }
    }
    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        (0, Some(self.n))
    }
    #[inline]
    fn count(self) -> usize {
        self.iter.count()
    }
}

impl<B: BitBlock> Iterator for Difference<'_, B> {
    type Item = usize;

    #[inline]
    fn next(&mut self) -> Option<usize> {
        self.0.next()
    }
    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        self.0.size_hint()
    }
    #[inline]
    fn count(self) -> usize {
        self.0.count()
    }
}

impl<B: BitBlock> Iterator for SymmetricDifference<'_, B> {
    type Item = usize;

    #[inline]
    fn next(&mut self) -> Option<usize> {
        self.0.next()
    }
    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        self.0.size_hint()
    }
    #[inline]
    fn count(self) -> usize {
        self.0.count()
    }
}

impl<'a, B: BitBlock> IntoIterator for &'a BitSet<B> {
    type Item = usize;
    type IntoIter = Iter<'a, B>;

    fn into_iter(self) -> Iter<'a, B> {
        self.iter()
    }
}
