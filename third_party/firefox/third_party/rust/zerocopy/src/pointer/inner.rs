// Copyright 2024 The Fuchsia Authors
// Licensed under a BSD-style license <LICENSE-BSD>, Apache License, Version 2.0
// <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
// license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.

use core::{marker::PhantomData, mem, ops::Range, ptr::NonNull};

pub use _def::PtrInner;

#[allow(unused_imports)]
use crate::util::polyfills::NumExt as _;
use crate::{
    layout::{CastType, MetadataCastError},
    util::AsAddress,
    AlignmentError, CastError, KnownLayout, MetadataOf, SizeError, SplitAt,
};

mod _def {
    use super::*;
    /// The inner pointer stored inside a [`Ptr`][crate::Ptr].
    ///
    /// `PtrInner<'a, T>` is [covariant] in `'a` and invariant in `T`.
    ///
    /// [covariant]: https://doc.rust-lang.org/reference/subtyping.html
    #[allow(missing_debug_implementations)]
    pub struct PtrInner<'a, T>
    where
        T: ?Sized,
    {
        /// # Invariants
        ///
        /// 0. If `ptr`'s referent is not zero sized, then `ptr` has valid
        ///    provenance for its referent, which is entirely contained in some
        ///    Rust allocation, `A`.
        /// 1. If `ptr`'s referent is not zero sized, `A` is guaranteed to live
        ///    for at least `'a`.
        ///
        /// # Postconditions
        ///
        /// By virtue of these invariants, code may assume the following, which
        /// are logical implications of the invariants:
        /// - `ptr`'s referent is not larger than `isize::MAX` bytes \[1\]
        /// - `ptr`'s referent does not wrap around the address space \[1\]
        ///
        /// \[1\] Per <https://doc.rust-lang.org/1.85.0/std/ptr/index.html#allocated-object>:
        ///
        ///   For any allocated object with `base` address, `size`, and a set of
        ///   `addresses`, the following are guaranteed:
        ///   ...
        ///   - `size <= isize::MAX`
        ///
        ///   As a consequence of these guarantees, given any address `a` within
        ///   the set of addresses of an allocated object:
        ///   ...
        ///   - It is guaranteed that, given `o = a - base` (i.e., the offset of
        ///     `a` within the allocated object), `base + o` will not wrap around
        ///     the address space (in other words, will not overflow `usize`)
        ptr: NonNull<T>,
        _marker: PhantomData<&'a core::cell::UnsafeCell<T>>,
    }

    impl<'a, T: 'a + ?Sized> Copy for PtrInner<'a, T> {}
    impl<'a, T: 'a + ?Sized> Clone for PtrInner<'a, T> {
        #[inline(always)]
        fn clone(&self) -> PtrInner<'a, T> {
            *self
        }
    }

    impl<'a, T: 'a + ?Sized> PtrInner<'a, T> {
        /// Constructs a `Ptr` from a [`NonNull`].
        ///
        /// # Safety
        ///
        /// The caller promises that:
        ///
        /// 0. If `ptr`'s referent is not zero sized, then `ptr` has valid
        ///    provenance for its referent, which is entirely contained in some
        ///    Rust allocation, `A`.
        /// 1. If `ptr`'s referent is not zero sized, `A` is guaranteed to live
        ///    for at least `'a`.
        #[inline(always)]
        #[must_use]
        pub const unsafe fn new(ptr: NonNull<T>) -> PtrInner<'a, T> {
            Self { ptr, _marker: PhantomData }
        }

        /// Converts this `PtrInner<T>` to a [`NonNull<T>`].
        ///
        /// Note that this method does not consume `self`. The caller should
        /// watch out for `unsafe` code which uses the returned `NonNull` in a
        /// way that violates the safety invariants of `self`.
        #[inline(always)]
        #[must_use]
        pub const fn as_non_null(&self) -> NonNull<T> {
            self.ptr
        }
    }
}

impl<'a, T: ?Sized> PtrInner<'a, T> {
    /// Constructs a `PtrInner` from a reference.
    #[inline]
    pub(crate) fn from_ref(ptr: &'a T) -> Self {
        let ptr = NonNull::from(ptr);
        unsafe { Self::new(ptr) }
    }

    /// Constructs a `PtrInner` from a mutable reference.
    #[inline]
    pub(crate) fn from_mut(ptr: &'a mut T) -> Self {
        let ptr = NonNull::from(ptr);
        unsafe { Self::new(ptr) }
    }

    #[must_use]
    #[inline(always)]
    pub fn cast_sized<U>(self) -> PtrInner<'a, U>
    where
        T: Sized,
    {
        static_assert!(T, U => mem::size_of::<T>() >= mem::size_of::<U>());
        unsafe { self.cast() }
    }

    /// # Safety
    ///
    /// `U` must not be larger than the size of `self`'s referent.
    #[must_use]
    #[inline(always)]
    pub unsafe fn cast<U>(self) -> PtrInner<'a, U> {
        let ptr = self.as_non_null().cast::<U>();

        unsafe { PtrInner::new(ptr) }
    }
}

#[allow(clippy::needless_lifetimes)]
impl<'a, T> PtrInner<'a, T>
where
    T: ?Sized + KnownLayout,
{
    /// Extracts the metadata of this `ptr`.
    pub(crate) fn meta(self) -> MetadataOf<T> {
        let meta = T::pointer_to_metadata(self.as_non_null().as_ptr());
        unsafe { MetadataOf::new_unchecked(meta) }
    }

    /// Produces a `PtrInner` with the same address and provenance as `self` but
    /// the given `meta`.
    ///
    /// # Safety
    ///
    /// The caller promises that if `self`'s referent is not zero sized, then
    /// a pointer constructed from its address with the given `meta` metadata
    /// will address a subset of the allocation pointed to by `self`.
    #[inline]
    pub(crate) unsafe fn with_meta(self, meta: T::PointerMetadata) -> Self
    where
        T: KnownLayout,
    {
        let raw = T::raw_from_ptr_len(self.as_non_null().cast(), meta);

        unsafe { PtrInner::new(raw) }
    }

    pub(crate) fn as_bytes(self) -> PtrInner<'a, [u8]> {
        let ptr = self.as_non_null();
        let bytes = match T::size_of_val_raw(ptr) {
            Some(bytes) => bytes,
            None => unsafe { core::hint::unreachable_unchecked() },
        };

        let ptr = core::ptr::slice_from_raw_parts_mut(ptr.cast::<u8>().as_ptr(), bytes);

        let ptr = unsafe { NonNull::new_unchecked(ptr) };

        unsafe { PtrInner::new(ptr) }
    }
}

#[allow(clippy::needless_lifetimes)]
impl<'a, T> PtrInner<'a, T>
where
    T: ?Sized + KnownLayout<PointerMetadata = usize>,
{
    /// Splits `T` in two.
    ///
    /// # Safety
    ///
    /// The caller promises that:
    ///  - `l_len.get() <= self.meta()`.
    ///
    /// ## (Non-)Overlap
    ///
    /// Given `let (left, right) = ptr.split_at(l_len)`, it is guaranteed that
    /// `left` and `right` are contiguous and non-overlapping if
    /// `l_len.padding_needed_for() == 0`. This is true for all `[T]`.
    ///
    /// If `l_len.padding_needed_for() != 0`, then the left pointer will overlap
    /// the right pointer to satisfy `T`'s padding requirements.
    pub(crate) unsafe fn split_at_unchecked(
        self,
        l_len: crate::util::MetadataOf<T>,
    ) -> (Self, PtrInner<'a, [T::Elem]>)
    where
        T: SplitAt,
    {
        let l_len = l_len.get();

        let left = unsafe { self.with_meta(l_len) };

        let right = self.trailing_slice();
        let right = unsafe { right.slice_unchecked(l_len..self.meta().get()) };

        (left, right)
    }

    /// Produces the trailing slice of `self`.
    pub(crate) fn trailing_slice(self) -> PtrInner<'a, [T::Elem]>
    where
        T: SplitAt,
    {
        let offset = crate::trailing_slice_layout::<T>().offset;

        let bytes = self.as_non_null().cast::<u8>().as_ptr();

        let bytes = unsafe { bytes.add(offset) };

        let bytes = unsafe { NonNull::new_unchecked(bytes) };

        let ptr = KnownLayout::raw_from_ptr_len(bytes, self.meta().get());

        unsafe { PtrInner::new(ptr) }
    }
}

#[allow(clippy::needless_lifetimes)]
impl<'a, T> PtrInner<'a, [T]> {
    /// Creates a pointer which addresses the given `range` of self.
    ///
    /// # Safety
    ///
    /// `range` is a valid range (`start <= end`) and `end <= self.meta()`.
    pub(crate) unsafe fn slice_unchecked(self, range: Range<usize>) -> Self {
        let base = self.as_non_null().cast::<T>().as_ptr();

        let base = unsafe { base.add(range.start) };

        #[allow(unstable_name_collisions)]
        let len = unsafe { range.end.unchecked_sub(range.start) };

        let ptr = core::ptr::slice_from_raw_parts_mut(base, len);

        let ptr = unsafe { NonNull::new_unchecked(ptr) };

        unsafe { PtrInner::new(ptr) }
    }

    /// Iteratively projects the elements `PtrInner<T>` from `PtrInner<[T]>`.
    pub(crate) fn iter(&self) -> impl Iterator<Item = PtrInner<'a, T>> {
        let base = self.as_non_null().cast::<T>().as_ptr();
        (0..self.meta().get()).map(move |i| {

            let elem = unsafe { base.add(i) };

            let elem = unsafe { NonNull::new_unchecked(elem) };

            unsafe { PtrInner::new(elem) }
        })
    }
}

impl<'a, T, const N: usize> PtrInner<'a, [T; N]> {
    /// Casts this pointer-to-array into a slice.
    ///
    /// # Safety
    ///
    /// Callers may assume that the returned `PtrInner` references the same
    /// address and length as `self`.
    #[allow(clippy::wrong_self_convention)]
    pub(crate) fn as_slice(self) -> PtrInner<'a, [T]> {
        let start = self.as_non_null().cast::<T>().as_ptr();
        let slice = core::ptr::slice_from_raw_parts_mut(start, N);
        let slice = unsafe { NonNull::new_unchecked(slice) };
        unsafe { PtrInner::new(slice) }
    }
}

impl<'a> PtrInner<'a, [u8]> {
    /// Attempts to cast `self` to a `U` using the given cast type.
    ///
    /// If `U` is a slice DST and pointer metadata (`meta`) is provided, then
    /// the cast will only succeed if it would produce an object with the given
    /// metadata.
    ///
    /// Returns `None` if the resulting `U` would be invalidly-aligned, if no
    /// `U` can fit in `self`, or if the provided pointer metadata describes an
    /// invalid instance of `U`. On success, returns a pointer to the
    /// largest-possible `U` which fits in `self`.
    ///
    /// # Safety
    ///
    /// The caller may assume that this implementation is correct, and may rely
    /// on that assumption for the soundness of their code. In particular, the
    /// caller may assume that, if `try_cast_into` returns `Some((ptr,
    /// remainder))`, then `ptr` and `remainder` refer to non-overlapping byte
    /// ranges within `self`, and that `ptr` and `remainder` entirely cover
    /// `self`. Finally:
    /// - If this is a prefix cast, `ptr` has the same address as `self`.
    /// - If this is a suffix cast, `remainder` has the same address as `self`.
    #[inline]
    pub(crate) fn try_cast_into<U>(
        self,
        cast_type: CastType,
        meta: Option<U::PointerMetadata>,
    ) -> Result<(PtrInner<'a, U>, PtrInner<'a, [u8]>), CastError<Self, U>>
    where
        U: 'a + ?Sized + KnownLayout,
    {
        let maybe_metadata = MetadataOf::<U>::validate_cast_and_convert_metadata(
            AsAddress::addr(self.as_non_null().as_ptr()),
            self.meta(),
            cast_type,
            meta,
        );

        let (elems, split_at) = match maybe_metadata {
            Ok((elems, split_at)) => (elems, split_at),
            Err(MetadataCastError::Alignment) => {
                let err = unsafe { AlignmentError::<_, U>::new_unchecked(self) };
                return Err(CastError::Alignment(err));
            }
            Err(MetadataCastError::Size) => return Err(CastError::Size(SizeError::new(self))),
        };

        let (l_slice, r_slice) = unsafe { self.split_at_unchecked(split_at) };

        let (target, remainder) = match cast_type {
            CastType::Prefix => (l_slice, r_slice),
            CastType::Suffix => (r_slice, l_slice),
        };

        let base = target.as_non_null().cast::<u8>();

        let ptr = U::raw_from_ptr_len(base, elems.get());

        Ok((unsafe { PtrInner::new(ptr) }, remainder))
    }
}
