// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

#![allow(clippy::unwrap_used)]
#![allow(clippy::expect_used)]
#![allow(clippy::indexing_slicing)]
#![allow(clippy::panic)]

use super::*;
use crate::ule::*;
use alloc::vec::Vec;
use core::any;
use core::convert::TryInto;
use core::marker::PhantomData;
use core::ops::Deref;
use core::ops::Range;
use core::{fmt, ptr, slice};

use super::components::IntegerULE;

/// A fully-owned [`VarZeroVec`]. This type has no lifetime but has the same
/// internal buffer representation of [`VarZeroVec`], making it cheaply convertible to
/// [`VarZeroVec`] and [`VarZeroSlice`].
///
/// The `F` type parameter is a [`VarZeroVecFormat`] (see its docs for more details), which can be used to select the
/// precise format of the backing buffer with various size and performance tradeoffs. It defaults to [`Index16`].
pub struct VarZeroVecOwned<T: ?Sized, F = Index16> {
    marker1: PhantomData<T>,
    marker2: PhantomData<F>,
    entire_slice: Vec<u8>,
}

impl<T: ?Sized, F> Clone for VarZeroVecOwned<T, F> {
    fn clone(&self) -> Self {
        VarZeroVecOwned {
            marker1: PhantomData,
            marker2: PhantomData,
            entire_slice: self.entire_slice.clone(),
        }
    }
}

#[derive(PartialEq)]
enum ShiftType {
    Insert,
    Replace,
    Remove,
}

impl<T: VarULE + ?Sized, F: VarZeroVecFormat> Deref for VarZeroVecOwned<T, F> {
    type Target = VarZeroSlice<T, F>;
    fn deref(&self) -> &VarZeroSlice<T, F> {
        self.as_slice()
    }
}

impl<T: VarULE + ?Sized, F> VarZeroVecOwned<T, F> {
    /// Construct an empty VarZeroVecOwned
    pub fn new() -> Self {
        Self {
            marker1: PhantomData,
            marker2: PhantomData,
            entire_slice: Vec::new(),
        }
    }
}

impl<T: VarULE + ?Sized, F: VarZeroVecFormat> VarZeroVecOwned<T, F> {
    /// Construct a VarZeroVecOwned from a [`VarZeroSlice`] by cloning the internal data
    pub fn from_slice(slice: &VarZeroSlice<T, F>) -> Self {
        Self {
            marker1: PhantomData,
            marker2: PhantomData,
            entire_slice: slice.as_bytes().into(),
        }
    }

    /// Construct a VarZeroVecOwned from a list of elements
    pub fn try_from_elements<A>(elements: &[A]) -> Result<Self, &'static str>
    where
        A: EncodeAsVarULE<T>,
    {
        Ok(if elements.is_empty() {
            Self::from_slice(VarZeroSlice::new_empty())
        } else {
            Self {
                marker1: PhantomData,
                marker2: PhantomData,
                entire_slice: components::get_serializable_bytes_non_empty::<T, A, F>(elements)
                    .ok_or(F::Index::TOO_LARGE_ERROR)?,
            }
        })
    }

    /// Obtain this `VarZeroVec` as a [`VarZeroSlice`]
    pub fn as_slice(&self) -> &VarZeroSlice<T, F> {
        let slice: &[u8] = &self.entire_slice;
        unsafe {
            VarZeroSlice::from_bytes_unchecked(slice)
        }
    }

    /// Try to allocate a buffer with enough capacity for `capacity`
    /// elements. Since `T` can take up an arbitrary size this will
    /// just allocate enough space for 4-byte Ts
    pub(crate) fn with_capacity(capacity: usize) -> Self {
        Self {
            marker1: PhantomData,
            marker2: PhantomData,
            entire_slice: Vec::with_capacity(capacity * (F::Index::SIZE + 4)),
        }
    }

    /// Try to reserve space for `capacity`
    /// elements. Since `T` can take up an arbitrary size this will
    /// just allocate enough space for 4-byte Ts
    pub(crate) fn reserve(&mut self, capacity: usize) {
        self.entire_slice.reserve(capacity * (F::Index::SIZE + 4))
    }

    /// Get the position of a specific element in the data segment.
    ///
    /// If `idx == self.len()`, it will return the size of the data segment (where a new element would go).
    ///
    /// ## Safety
    /// `idx <= self.len()` and `self.as_encoded_bytes()` is well-formed.
    unsafe fn element_position_unchecked(&self, idx: usize) -> usize {
        let len = self.len();
        let out = if idx == len {
            self.entire_slice.len() - F::Len::SIZE - (F::Index::SIZE * (len - 1))
        } else if let Some(idx) = self.index_data(idx) {
            idx.iule_to_usize()
        } else {
            0
        };
        debug_assert!(out + F::Len::SIZE + (len - 1) * F::Index::SIZE <= self.entire_slice.len());
        out
    }

    /// Get the range of a specific element in the data segment.
    ///
    /// ## Safety
    /// `idx < self.len()` and `self.as_encoded_bytes()` is well-formed.
    unsafe fn element_range_unchecked(&self, idx: usize) -> core::ops::Range<usize> {
        let start = self.element_position_unchecked(idx);
        let end = self.element_position_unchecked(idx + 1);
        debug_assert!(start <= end, "{start} > {end}");
        start..end
    }

    /// Set the number of elements in the list without any checks.
    ///
    /// ## Safety
    /// No safe functions may be called until `self.as_encoded_bytes()` is well-formed.
    unsafe fn set_len(&mut self, len: usize) {
        assert!(len <= F::Len::MAX_VALUE as usize);
        let len_bytes = len.to_le_bytes();
        let len_ule = F::Len::iule_from_usize(len).expect(F::Len::TOO_LARGE_ERROR);
        self.entire_slice[0..F::Len::SIZE].copy_from_slice(ULE::slice_as_bytes(&[len_ule]));
        assert_eq!(len_bytes[F::Len::SIZE..].iter().sum::<u8>(), 0);
    }

    /// Get the range in the full data for a given index. Returns None for index 0
    /// since there is no stored index for it.
    fn index_range(index: usize) -> Option<Range<usize>> {
        let index_minus_one = index.checked_sub(1)?;
        let pos = F::Len::SIZE + F::Index::SIZE * index_minus_one;
        Some(pos..pos + F::Index::SIZE)
    }

    /// Return the raw bytes representing the given `index`. Returns None when given index 0
    ///
    /// ## Safety
    /// The index must be valid, and self.as_encoded_bytes() must be well-formed
    unsafe fn index_data(&self, index: usize) -> Option<&F::Index> {
        let index_range = Self::index_range(index)?;
        Some(&F::Index::slice_from_bytes_unchecked(&self.entire_slice[index_range])[0])
    }

    /// Return the mutable slice representing the given `index`. Returns None when given index 0
    ///
    /// ## Safety
    /// The index must be valid. self.as_encoded_bytes() must have allocated space
    /// for this index, but need not have its length appropriately set.
    unsafe fn index_data_mut(&mut self, index: usize) -> Option<&mut F::Index> {
        let ptr = self.entire_slice.as_mut_ptr();
        let range = Self::index_range(index)?;

        let data = slice::from_raw_parts_mut(ptr.add(range.start), F::Index::SIZE);
        Some(&mut F::Index::iule_from_bytes_unchecked_mut(data)[0])
    }

    /// Shift the indices starting with and after `starting_index` by the provided `amount`.
    ///
    /// ## Panics
    /// Should never be called with a starting index of 0, since that index cannot be shifted.
    ///
    /// ## Safety
    /// Adding `amount` to each index after `starting_index` must not result in the slice from becoming malformed.
    /// The length of the slice must be correctly set.
    unsafe fn shift_indices(&mut self, starting_index: usize, amount: i32) {
        let normalized_idx = starting_index
            .checked_sub(1)
            .expect("shift_indices called with a 0 starting index");
        let len = self.len();
        let indices = F::Index::iule_from_bytes_unchecked_mut(
            &mut self.entire_slice[F::Len::SIZE..F::Len::SIZE + F::Index::SIZE * (len - 1)],
        );
        for idx in &mut indices[normalized_idx..] {
            let mut new_idx = idx.iule_to_usize();
            if amount > 0 {
                new_idx = new_idx.checked_add(amount.try_into().unwrap()).unwrap();
            } else {
                new_idx = new_idx.checked_sub((-amount).try_into().unwrap()).unwrap();
            }
            *idx = F::Index::iule_from_usize(new_idx).expect(F::Index::TOO_LARGE_ERROR);
        }
    }

    /// Get this [`VarZeroVecOwned`] as a borrowed [`VarZeroVec`]
    ///
    /// If you wish to repeatedly call methods on this [`VarZeroVecOwned`],
    /// it is more efficient to perform this conversion first
    pub fn as_varzerovec<'a>(&'a self) -> VarZeroVec<'a, T, F> {
        self.as_slice().into()
    }

    /// Empty the vector
    pub fn clear(&mut self) {
        self.entire_slice.clear()
    }

    /// Consume this vector and return the backing buffer
    #[inline]
    pub fn into_bytes(self) -> Vec<u8> {
        self.entire_slice
    }

    /// Invalidate and resize the data at an index, optionally inserting or removing the index.
    /// Also updates affected indices and the length.
    ///
    /// `new_size` is the encoded byte size of the element that is going to be inserted
    ///
    /// Returns a slice to the new element data - it doesn't contain uninitialized data but its value is indeterminate.
    ///
    /// ## Safety
    /// - `index` must be a valid index, or, if `shift_type == ShiftType::Insert`, `index == self.len()` is allowed.
    /// - `new_size` musn't result in the data segment growing larger than `F::Index::MAX_VALUE`.
    unsafe fn shift(&mut self, index: usize, new_size: usize, shift_type: ShiftType) -> &mut [u8] {
        let len = self.len();
        let slice_len = self.entire_slice.len();

        let prev_element = match shift_type {
            ShiftType::Insert => {
                let pos = self.element_position_unchecked(index);
                pos..pos
            }
            _ => self.element_range_unchecked(index),
        };

        let index_shift: i64 = match shift_type {
            ShiftType::Insert => F::Index::SIZE as i64,
            ShiftType::Replace => 0,
            ShiftType::Remove => -(F::Index::SIZE as i64),
        };
        let shift: i64 =
            new_size as i64 - (prev_element.end - prev_element.start) as i64 + index_shift;
        let new_slice_len = slice_len.wrapping_add(shift as usize);
        if shift > 0 {
            if new_slice_len > F::Index::MAX_VALUE as usize {
                panic!(
                    "Attempted to grow VarZeroVec to an encoded size that does not fit within the length size used by {}",
                    any::type_name::<F>()
                );
            }
            self.entire_slice.resize(new_slice_len, 0);
        }

        {
            let slice_range = self.entire_slice.as_mut_ptr_range();
            let indices_start = slice_range.start.add(F::Len::SIZE);
            let old_slice_end = slice_range.start.add(slice_len);
            let data_start = indices_start.add((len - 1) * F::Index::SIZE);
            let prev_element_p =
                data_start.add(prev_element.start)..data_start.add(prev_element.end);

            let index_range = if let Some(index_minus_one) = index.checked_sub(1) {
                let index_start = indices_start.add(F::Index::SIZE * index_minus_one);
                Some(index_start..index_start.add(F::Index::SIZE))
            } else {
                None
            };

            unsafe fn shift_bytes(block: Range<*const u8>, to: *mut u8) {
                debug_assert!(block.end >= block.start);
                ptr::copy(block.start, to, block.end.offset_from(block.start) as usize);
            }

            if shift_type == ShiftType::Remove {
                if let Some(ref index_range) = index_range {
                    shift_bytes(index_range.end..prev_element_p.start, index_range.start);
                } else {
                    shift_bytes(
                        indices_start.add(F::Index::SIZE)..prev_element_p.start,
                        indices_start,
                    )
                }
            }

            shift_bytes(
                prev_element_p.end..old_slice_end,
                prev_element_p
                    .start
                    .offset((new_size as i64 + index_shift) as isize),
            );

            let first_affected_index = match shift_type {
                ShiftType::Insert => {
                    if let Some(index_range) = index_range {
                        shift_bytes(index_range.start..prev_element_p.start, index_range.end);
                        let index_data = self
                            .index_data_mut(index)
                            .expect("If index_range is some, index is > 0 and should not panic in index_data_mut");
                        *index_data = F::Index::iule_from_usize(prev_element.start)
                            .expect(F::Index::TOO_LARGE_ERROR);
                    } else {
                        shift_bytes(
                            indices_start..prev_element_p.start,
                            indices_start.add(F::Index::SIZE),
                        );
                        let index_data = self
                            .index_data_mut(1)
                            .expect("Should be able to write to index 1");
                        *index_data = F::Index::iule_from_usize(0).expect("0 is always valid!");
                    }

                    self.set_len(len + 1);
                    index + 1
                }
                ShiftType::Remove => {
                    self.set_len(len - 1);
                    if index == 0 {
                        index + 1
                    } else {
                        index
                    }
                }
                ShiftType::Replace => index + 1,
            };

            self.entire_slice.set_len(new_slice_len);
            self.shift_indices(first_affected_index, (shift - index_shift) as i32);
        };

        debug_assert!(self.verify_integrity());

        let element_pos = F::Len::SIZE
            + (self.len() - 1) * F::Index::SIZE
            + self.element_position_unchecked(index);
        &mut self.entire_slice[element_pos..element_pos + new_size]
    }

    /// Checks the internal invariants of the vec to ensure safe code will not cause UB.
    /// Returns whether integrity was verified.
    ///
    /// Note: an index is valid if it doesn't point to data past the end of the slice and is
    /// less than or equal to all future indices. The length of the index segment is not part of each index.
    fn verify_integrity(&self) -> bool {
        if self.is_empty() {
            if self.entire_slice.is_empty() {
                return true;
            } else {
                panic!(
                    "VarZeroVecOwned integrity: Found empty VarZeroVecOwned with a nonempty slice"
                );
            }
        }
        let len = unsafe {
            <F::Len as ULE>::slice_from_bytes_unchecked(&self.entire_slice[..F::Len::SIZE])[0]
                .iule_to_usize()
        };
        if len == 0 {
            panic!("VarZeroVecOwned integrity: Found empty VarZeroVecOwned with a nonempty slice");
        }
        if self.entire_slice.len() < F::Len::SIZE + (len - 1) * F::Index::SIZE {
            panic!("VarZeroVecOwned integrity: Not enough room for the indices");
        }
        let data_len = self.entire_slice.len() - F::Len::SIZE - (len - 1) * F::Index::SIZE;
        if data_len > F::Index::MAX_VALUE as usize {
            panic!("VarZeroVecOwned integrity: Data segment is too long");
        }

        let indices = unsafe {
            F::Index::slice_from_bytes_unchecked(
                &self.entire_slice[F::Len::SIZE..F::Len::SIZE + (len - 1) * F::Index::SIZE],
            )
        };
        for idx in indices {
            if idx.iule_to_usize() > data_len {
                panic!("VarZeroVecOwned integrity: Indices must not point past the data segment");
            }
        }
        for window in indices.windows(2) {
            if window[0].iule_to_usize() > window[1].iule_to_usize() {
                panic!("VarZeroVecOwned integrity: Indices must be in non-decreasing order");
            }
        }
        true
    }

    /// Insert an element at the end of this vector
    pub fn push<A: EncodeAsVarULE<T> + ?Sized>(&mut self, element: &A) {
        self.insert(self.len(), element)
    }

    /// Insert an element at index `idx`
    pub fn insert<A: EncodeAsVarULE<T> + ?Sized>(&mut self, index: usize, element: &A) {
        let len = self.len();
        if index > len {
            panic!("Called out-of-bounds insert() on VarZeroVec, index {index} len {len}");
        }

        let value_len = element.encode_var_ule_len();

        if len == 0 {
            let header_len = F::Len::SIZE; 
            let cap = header_len + value_len;
            self.entire_slice.resize(cap, 0);
            self.entire_slice[0] = 1; 
            element.encode_var_ule_write(&mut self.entire_slice[header_len..]);
            return;
        }

        assert!(value_len < F::Index::MAX_VALUE as usize);
        unsafe {
            let place = self.shift(index, value_len, ShiftType::Insert);
            element.encode_var_ule_write(place);
        }
    }

    /// Remove the element at index `idx`
    pub fn remove(&mut self, index: usize) {
        let len = self.len();
        if index >= len {
            panic!("Called out-of-bounds remove() on VarZeroVec, index {index} len {len}");
        }
        if len == 1 {
            self.entire_slice.clear();
            return;
        }
        unsafe {
            self.shift(index, 0, ShiftType::Remove);
        }
    }

    /// Replace the element at index `idx` with another
    pub fn replace<A: EncodeAsVarULE<T> + ?Sized>(&mut self, index: usize, element: &A) {
        let len = self.len();
        if index >= len {
            panic!("Called out-of-bounds replace() on VarZeroVec, index {index} len {len}");
        }

        let value_len = element.encode_var_ule_len();

        assert!(value_len < F::Index::MAX_VALUE as usize);
        unsafe {
            let place = self.shift(index, value_len, ShiftType::Replace);
            element.encode_var_ule_write(place);
        }
    }
}

impl<T: VarULE + ?Sized, F: VarZeroVecFormat> fmt::Debug for VarZeroVecOwned<T, F>
where
    T: fmt::Debug,
{
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        VarZeroSlice::fmt(self, f)
    }
}

impl<T: VarULE + ?Sized, F> Default for VarZeroVecOwned<T, F> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T, A, F> PartialEq<&'_ [A]> for VarZeroVecOwned<T, F>
where
    T: VarULE + ?Sized,
    T: PartialEq,
    A: AsRef<T>,
    F: VarZeroVecFormat,
{
    #[inline]
    fn eq(&self, other: &&[A]) -> bool {
        self.iter().eq(other.iter().map(|t| t.as_ref()))
    }
}

impl<'a, T: ?Sized + VarULE, F: VarZeroVecFormat> From<&'a VarZeroSlice<T, F>>
    for VarZeroVecOwned<T, F>
{
    fn from(other: &'a VarZeroSlice<T, F>) -> Self {
        Self::from_slice(other)
    }
}
