// Copyright 2023 The Fuchsia Authors
// Licensed under a BSD-style license <LICENSE-BSD>, Apache License, Version 2.0
// <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
// license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.

use core::{
    fmt::{Debug, Formatter},
    marker::PhantomData,
};

use crate::{
    pointer::{
        inner::PtrInner,
        invariant::*,
        transmute::{MutationCompatible, SizeEq, TransmuteFromPtr},
    },
    AlignmentError, CastError, CastType, KnownLayout, SizeError, TryFromBytes, ValidityError,
};

/// Module used to gate access to [`Ptr`]'s fields.
mod def {
    #[cfg(doc)]
    use super::super::invariant;
    use super::*;

    /// A raw pointer with more restrictions.
    ///
    /// `Ptr<T>` is similar to [`NonNull<T>`], but it is more restrictive in the
    /// following ways (note that these requirements only hold of non-zero-sized
    /// referents):
    /// - It must derive from a valid allocation.
    /// - It must reference a byte range which is contained inside the
    ///   allocation from which it derives.
    ///   - As a consequence, the byte range it references must have a size
    ///     which does not overflow `isize`.
    ///
    /// Depending on how `Ptr` is parameterized, it may have additional
    /// invariants:
    /// - `ptr` conforms to the aliasing invariant of
    ///   [`I::Aliasing`](invariant::Aliasing).
    /// - `ptr` conforms to the alignment invariant of
    ///   [`I::Alignment`](invariant::Alignment).
    /// - `ptr` conforms to the validity invariant of
    ///   [`I::Validity`](invariant::Validity).
    ///
    /// `Ptr<'a, T>` is [covariant] in `'a` and invariant in `T`.
    ///
    /// [`NonNull<T>`]: core::ptr::NonNull
    /// [covariant]: https://doc.rust-lang.org/reference/subtyping.html
    pub struct Ptr<'a, T, I>
    where
        T: ?Sized,
        I: Invariants,
    {
        /// # Invariants
        ///
        /// 0. `ptr` conforms to the aliasing invariant of
        ///    [`I::Aliasing`](invariant::Aliasing).
        /// 1. `ptr` conforms to the alignment invariant of
        ///    [`I::Alignment`](invariant::Alignment).
        /// 2. `ptr` conforms to the validity invariant of
        ///    [`I::Validity`](invariant::Validity).
        ptr: PtrInner<'a, T>,
        _invariants: PhantomData<I>,
    }

    impl<'a, T, I> Ptr<'a, T, I>
    where
        T: 'a + ?Sized,
        I: Invariants,
    {
        /// Constructs a new `Ptr` from a [`PtrInner`].
        ///
        /// # Safety
        ///
        /// The caller promises that:
        ///
        /// 0. `ptr` conforms to the aliasing invariant of
        ///    [`I::Aliasing`](invariant::Aliasing).
        /// 1. `ptr` conforms to the alignment invariant of
        ///    [`I::Alignment`](invariant::Alignment).
        /// 2. `ptr` conforms to the validity invariant of
        ///    [`I::Validity`](invariant::Validity).
        pub(crate) unsafe fn from_inner(ptr: PtrInner<'a, T>) -> Ptr<'a, T, I> {
            Self { ptr, _invariants: PhantomData }
        }

        /// Converts this `Ptr<T>` to a [`PtrInner<T>`].
        ///
        /// Note that this method does not consume `self`. The caller should
        /// watch out for `unsafe` code which uses the returned value in a way
        /// that violates the safety invariants of `self`.
        pub(crate) fn as_inner(&self) -> PtrInner<'a, T> {
            self.ptr
        }
    }
}

#[allow(unreachable_pub)] 
pub use def::Ptr;

/// External trait implementations on [`Ptr`].
mod _external {
    use super::*;

    /// SAFETY: Shared pointers are safely `Copy`. `Ptr`'s other invariants
    /// (besides aliasing) are unaffected by the number of references that exist
    /// to `Ptr`'s referent. The notable cases are:
    /// - Alignment is a property of the referent type (`T`) and the address,
    ///   both of which are unchanged
    /// - Let `S(T, V)` be the set of bit values permitted to appear in the
    ///   referent of a `Ptr<T, I: Invariants<Validity = V>>`. Since this copy
    ///   does not change `I::Validity` or `T`, `S(T, I::Validity)` is also
    ///   unchanged.
    ///   
    ///   We are required to guarantee that the referents of the original `Ptr`
    ///   and of the copy (which, of course, are actually the same since they
    ///   live in the same byte address range) both remain in the set `S(T,
    ///   I::Validity)`. Since this invariant holds on the original `Ptr`, it
    ///   cannot be violated by the original `Ptr`, and thus the original `Ptr`
    ///   cannot be used to violate this invariant on the copy. The inverse
    ///   holds as well.
    impl<'a, T, I> Copy for Ptr<'a, T, I>
    where
        T: 'a + ?Sized,
        I: Invariants<Aliasing = Shared>,
    {
    }

    /// SAFETY: See the safety comment on `Copy`.
    impl<'a, T, I> Clone for Ptr<'a, T, I>
    where
        T: 'a + ?Sized,
        I: Invariants<Aliasing = Shared>,
    {
        #[inline]
        fn clone(&self) -> Self {
            *self
        }
    }

    impl<'a, T, I> Debug for Ptr<'a, T, I>
    where
        T: 'a + ?Sized,
        I: Invariants,
    {
        #[inline]
        fn fmt(&self, f: &mut Formatter<'_>) -> core::fmt::Result {
            self.as_inner().as_non_null().fmt(f)
        }
    }
}

/// Methods for converting to and from `Ptr` and Rust's safe reference types.
mod _conversions {
    use super::*;

    /// `&'a T` → `Ptr<'a, T>`
    impl<'a, T> Ptr<'a, T, (Shared, Aligned, Valid)>
    where
        T: 'a + ?Sized,
    {
        /// Constructs a `Ptr` from a shared reference.
        #[doc(hidden)]
        #[inline]
        pub fn from_ref(ptr: &'a T) -> Self {
            let inner = PtrInner::from_ref(ptr);
            unsafe { Self::from_inner(inner) }
        }
    }

    /// `&'a mut T` → `Ptr<'a, T>`
    impl<'a, T> Ptr<'a, T, (Exclusive, Aligned, Valid)>
    where
        T: 'a + ?Sized,
    {
        /// Constructs a `Ptr` from an exclusive reference.
        #[inline]
        pub(crate) fn from_mut(ptr: &'a mut T) -> Self {
            let inner = PtrInner::from_mut(ptr);
            unsafe { Self::from_inner(inner) }
        }
    }

    /// `Ptr<'a, T>` → `&'a T`
    impl<'a, T, I> Ptr<'a, T, I>
    where
        T: 'a + ?Sized,
        I: Invariants<Alignment = Aligned, Validity = Valid>,
        I::Aliasing: Reference,
    {
        /// Converts `self` to a shared reference.
        #[allow(clippy::wrong_self_convention)]
        pub(crate) fn as_ref(self) -> &'a T {
            let raw = self.as_inner().as_non_null();
            unsafe { raw.as_ref() }
        }
    }

    impl<'a, T, I> Ptr<'a, T, I>
    where
        T: 'a + ?Sized,
        I: Invariants,
        I::Aliasing: Reference,
    {
        /// Reborrows `self`, producing another `Ptr`.
        ///
        /// Since `self` is borrowed immutably, this prevents any mutable
        /// methods from being called on `self` as long as the returned `Ptr`
        /// exists.
        #[doc(hidden)]
        #[inline]
        #[allow(clippy::needless_lifetimes)] 
        pub fn reborrow<'b>(&'b mut self) -> Ptr<'b, T, I>
        where
            'a: 'b,
        {
            unsafe { Ptr::from_inner(self.as_inner()) }
        }
    }

    /// `Ptr<'a, T>` → `&'a mut T`
    impl<'a, T> Ptr<'a, T, (Exclusive, Aligned, Valid)>
    where
        T: 'a + ?Sized,
    {
        /// Converts `self` to a mutable reference.
        #[allow(clippy::wrong_self_convention)]
        pub(crate) fn as_mut(self) -> &'a mut T {
            let mut raw = self.as_inner().as_non_null();
            unsafe { raw.as_mut() }
        }
    }

    /// `Ptr<'a, T>` → `Ptr<'a, U>`
    impl<'a, T: ?Sized, I> Ptr<'a, T, I>
    where
        I: Invariants,
    {
        pub(crate) fn transmute<U, V, R>(self) -> Ptr<'a, U, (I::Aliasing, Unaligned, V)>
        where
            V: Validity,
            U: TransmuteFromPtr<T, I::Aliasing, I::Validity, V, R> + SizeEq<T> + ?Sized,
        {
            unsafe { self.transmute_unchecked(SizeEq::cast_from_raw) }
        }

        #[doc(hidden)]
        #[inline(always)]
        #[must_use]
        pub fn recall_validity<V, R>(self) -> Ptr<'a, T, (I::Aliasing, I::Alignment, V)>
        where
            V: Validity,
            T: TransmuteFromPtr<T, I::Aliasing, I::Validity, V, R>,
        {
            let ptr = unsafe { self.transmute_unchecked(SizeEq::cast_from_raw) };
            unsafe { ptr.assume_alignment::<I::Alignment>() }
        }

        /// Casts to a different (unsized) target type without checking interior
        /// mutability.
        ///
        /// Callers should prefer [`cast_unsized`] where possible.
        ///
        /// [`cast_unsized`]: Ptr::cast_unsized
        ///
        /// # Safety
        ///
        /// The caller promises that `u = cast(p)` is a pointer cast with the
        /// following properties:
        /// - `u` addresses a subset of the bytes addressed by `p`
        /// - `u` has the same provenance as `p`
        /// - If `I::Aliasing` is [`Shared`], it must not be possible for safe
        ///   code, operating on a `&T` and `&U` with the same referent
        ///   simultaneously, to cause undefined behavior
        /// - It is sound to transmute a pointer of type `T` with aliasing
        ///   `I::Aliasing` and validity `I::Validity` to a pointer of type `U`
        ///   with aliasing `I::Aliasing` and validity `V`. This is a subtle
        ///   soundness requirement that is a function of `T`, `U`,
        ///   `I::Aliasing`, `I::Validity`, and `V`, and may depend upon the
        ///   presence, absence, or specific location of `UnsafeCell`s in `T`
        ///   and/or `U`. See [`Validity`] for more details.
        #[doc(hidden)]
        #[inline]
        pub unsafe fn transmute_unchecked<U: ?Sized, V, F>(
            self,
            cast: F,
        ) -> Ptr<'a, U, (I::Aliasing, Unaligned, V)>
        where
            V: Validity,
            F: FnOnce(PtrInner<'a, T>) -> PtrInner<'a, U>,
        {
            let ptr = cast(self.as_inner());

            unsafe { Ptr::from_inner(ptr) }
        }
    }

    /// `Ptr<'a, T, (_, _, _)>` → `Ptr<'a, Unalign<T>, (_, Aligned, _)>`
    impl<'a, T, I> Ptr<'a, T, I>
    where
        I: Invariants,
    {
        /// Converts a `Ptr` an unaligned `T` into a `Ptr` to an aligned
        /// `Unalign<T>`.
        pub(crate) fn into_unalign(
            self,
        ) -> Ptr<'a, crate::Unalign<T>, (I::Aliasing, Aligned, I::Validity)> {
            let ptr = unsafe { self.transmute_unchecked(PtrInner::cast_sized) };
            ptr.bikeshed_recall_aligned()
        }
    }

    impl<'a, T, I> Ptr<'a, T, I>
    where
        T: ?Sized,
        I: Invariants<Validity = Valid>,
        I::Aliasing: Reference,
    {
        /// Reads the referent.
        #[must_use]
        #[inline]
        pub fn read_unaligned<R>(self) -> T
        where
            T: Copy,
            T: Read<I::Aliasing, R>,
        {
            (*self.into_unalign().as_ref()).into_inner()
        }

        /// Views the value as an aligned reference.
        ///
        /// This is only available if `T` is [`Unaligned`].
        #[must_use]
        #[inline]
        pub fn unaligned_as_ref(self) -> &'a T
        where
            T: crate::Unaligned,
        {
            self.bikeshed_recall_aligned().as_ref()
        }
    }
}

/// State transitions between invariants.
mod _transitions {
    use super::*;
    use crate::pointer::transmute::TryTransmuteFromPtr;

    impl<'a, T, I> Ptr<'a, T, I>
    where
        T: 'a + ?Sized,
        I: Invariants,
    {
        /// Returns a `Ptr` with [`Exclusive`] aliasing if `self` already has
        /// `Exclusive` aliasing, or generates a compile-time assertion failure.
        ///
        /// This allows code which is generic over aliasing to down-cast to a
        /// concrete aliasing.
        ///
        /// [`Exclusive`]: crate::pointer::invariant::Exclusive
        #[inline]
        pub(crate) fn into_exclusive_or_pme(
            self,
        ) -> Ptr<'a, T, (Exclusive, I::Alignment, I::Validity)> {
            trait AliasingExt: Aliasing {
                const IS_EXCL: bool;
            }

            impl<A: Aliasing> AliasingExt for A {
                const IS_EXCL: bool = {
                    const_assert!(Self::IS_EXCLUSIVE);
                    true
                };
            }

            assert!(I::Aliasing::IS_EXCL);

            unsafe { self.assume_exclusive() }
        }

        /// Assumes that `self` satisfies the invariants `H`.
        ///
        /// # Safety
        ///
        /// The caller promises that `self` satisfies the invariants `H`.
        unsafe fn assume_invariants<H: Invariants>(self) -> Ptr<'a, T, H> {
            unsafe { Ptr::from_inner(self.as_inner()) }
        }

        /// Helps the type system unify two distinct invariant types which are
        /// actually the same.
        pub(crate) fn unify_invariants<
            H: Invariants<Aliasing = I::Aliasing, Alignment = I::Alignment, Validity = I::Validity>,
        >(
            self,
        ) -> Ptr<'a, T, H> {
            unsafe { self.assume_invariants::<H>() }
        }

        /// Assumes that `self` satisfies the aliasing requirement of `A`.
        ///
        /// # Safety
        ///
        /// The caller promises that `self` satisfies the aliasing requirement
        /// of `A`.
        #[inline]
        pub(crate) unsafe fn assume_aliasing<A: Aliasing>(
            self,
        ) -> Ptr<'a, T, (A, I::Alignment, I::Validity)> {
            unsafe { self.assume_invariants() }
        }

        /// Assumes `self` satisfies the aliasing requirement of [`Exclusive`].
        ///
        /// # Safety
        ///
        /// The caller promises that `self` satisfies the aliasing requirement
        /// of `Exclusive`.
        ///
        /// [`Exclusive`]: crate::pointer::invariant::Exclusive
        #[inline]
        pub(crate) unsafe fn assume_exclusive(
            self,
        ) -> Ptr<'a, T, (Exclusive, I::Alignment, I::Validity)> {
            unsafe { self.assume_aliasing::<Exclusive>() }
        }

        /// Assumes that `self`'s referent is validly-aligned for `T` if
        /// required by `A`.
        ///
        /// # Safety
        ///
        /// The caller promises that `self`'s referent conforms to the alignment
        /// invariant of `T` if required by `A`.
        #[inline]
        pub(crate) unsafe fn assume_alignment<A: Alignment>(
            self,
        ) -> Ptr<'a, T, (I::Aliasing, A, I::Validity)> {
            unsafe { self.assume_invariants() }
        }

        /// Checks the `self`'s alignment at runtime, returning an aligned `Ptr`
        /// on success.
        pub(crate) fn try_into_aligned(
            self,
        ) -> Result<Ptr<'a, T, (I::Aliasing, Aligned, I::Validity)>, AlignmentError<Self, T>>
        where
            T: Sized,
        {
            if let Err(err) =
                crate::util::validate_aligned_to::<_, T>(self.as_inner().as_non_null())
            {
                return Err(err.with_src(self));
            }

            Ok(unsafe { self.assume_alignment::<Aligned>() })
        }

        /// Recalls that `self`'s referent is validly-aligned for `T`.
        #[inline]
        pub(crate) fn bikeshed_recall_aligned(
            self,
        ) -> Ptr<'a, T, (I::Aliasing, Aligned, I::Validity)>
        where
            T: crate::Unaligned,
        {
            unsafe { self.assume_alignment::<Aligned>() }
        }

        /// Assumes that `self`'s referent conforms to the validity requirement
        /// of `V`.
        ///
        /// # Safety
        ///
        /// The caller promises that `self`'s referent conforms to the validity
        /// requirement of `V`.
        #[doc(hidden)]
        #[must_use]
        #[inline]
        pub unsafe fn assume_validity<V: Validity>(
            self,
        ) -> Ptr<'a, T, (I::Aliasing, I::Alignment, V)> {
            unsafe { self.assume_invariants() }
        }

        /// A shorthand for `self.assume_validity<invariant::Initialized>()`.
        ///
        /// # Safety
        ///
        /// The caller promises to uphold the safety preconditions of
        /// `self.assume_validity<invariant::Initialized>()`.
        #[doc(hidden)]
        #[must_use]
        #[inline]
        pub unsafe fn assume_initialized(
            self,
        ) -> Ptr<'a, T, (I::Aliasing, I::Alignment, Initialized)> {
            unsafe { self.assume_validity::<Initialized>() }
        }

        /// A shorthand for `self.assume_validity<Valid>()`.
        ///
        /// # Safety
        ///
        /// The caller promises to uphold the safety preconditions of
        /// `self.assume_validity<Valid>()`.
        #[doc(hidden)]
        #[must_use]
        #[inline]
        pub unsafe fn assume_valid(self) -> Ptr<'a, T, (I::Aliasing, I::Alignment, Valid)> {
            unsafe { self.assume_validity::<Valid>() }
        }

        /// Recalls that `self`'s referent is initialized.
        #[doc(hidden)]
        #[must_use]
        #[inline]
        pub fn bikeshed_recall_initialized_from_bytes(
            self,
        ) -> Ptr<'a, T, (I::Aliasing, I::Alignment, Initialized)>
        where
            T: crate::IntoBytes + crate::FromBytes,
            I: Invariants<Validity = Valid>,
        {
            unsafe { self.assume_initialized() }
        }

        /// Recalls that `self`'s referent is initialized.
        #[doc(hidden)]
        #[must_use]
        #[inline]
        pub fn bikeshed_recall_initialized_immutable(
            self,
        ) -> Ptr<'a, T, (Shared, I::Alignment, Initialized)>
        where
            T: crate::IntoBytes + crate::Immutable,
            I: Invariants<Aliasing = Shared, Validity = Valid>,
        {
            unsafe { self.assume_initialized() }
        }

        /// Checks that `self`'s referent is validly initialized for `T`,
        /// returning a `Ptr` with `Valid` on success.
        ///
        /// # Panics
        ///
        /// This method will panic if
        /// [`T::is_bit_valid`][TryFromBytes::is_bit_valid] panics.
        ///
        /// # Safety
        ///
        /// On error, unsafe code may rely on this method's returned
        /// `ValidityError` containing `self`.
        #[inline]
        pub(crate) fn try_into_valid<R, S>(
            mut self,
        ) -> Result<Ptr<'a, T, (I::Aliasing, I::Alignment, Valid)>, ValidityError<Self, T>>
        where
            T: TryFromBytes
                + Read<I::Aliasing, R>
                + TryTransmuteFromPtr<T, I::Aliasing, I::Validity, Valid, S>,
            I::Aliasing: Reference,
            I: Invariants<Validity = Initialized>,
        {
            if T::is_bit_valid(self.reborrow().forget_aligned()) {
                Ok(unsafe { self.assume_valid() })
            } else {
                Err(ValidityError::new(self))
            }
        }

        /// Forgets that `self`'s referent is validly-aligned for `T`.
        #[doc(hidden)]
        #[must_use]
        #[inline]
        pub fn forget_aligned(self) -> Ptr<'a, T, (I::Aliasing, Unaligned, I::Validity)> {
            unsafe { self.assume_invariants() }
        }
    }
}

/// Casts of the referent type.
mod _casts {
    use super::*;

    impl<'a, T, I> Ptr<'a, T, I>
    where
        T: 'a + ?Sized,
        I: Invariants,
    {
        /// Casts to a different (unsized) target type without checking interior
        /// mutability.
        ///
        /// Callers should prefer [`cast_unsized`] where possible.
        ///
        /// [`cast_unsized`]: Ptr::cast_unsized
        ///
        /// # Safety
        ///
        /// The caller promises that `u = cast(p)` is a pointer cast with the
        /// following properties:
        /// - `u` addresses a subset of the bytes addressed by `p`
        /// - `u` has the same provenance as `p`
        /// - If `I::Aliasing` is [`Shared`], it must not be possible for safe
        ///   code, operating on a `&T` and `&U` with the same referent
        ///   simultaneously, to cause undefined behavior
        ///
        /// `cast_unsized_unchecked` guarantees that the pointer passed to
        /// `cast` will reference a byte sequence which is either contained
        /// inside a single allocated object or is zero sized. In either case,
        /// this means that its size will fit in an `isize` and it will not wrap
        /// around the address space.
        #[doc(hidden)]
        #[inline]
        pub unsafe fn cast_unsized_unchecked<U, F: FnOnce(PtrInner<'a, T>) -> PtrInner<'a, U>>(
            self,
            cast: F,
        ) -> Ptr<'a, U, (I::Aliasing, Unaligned, I::Validity)>
        where
            U: 'a + CastableFrom<T, I::Validity, I::Validity> + ?Sized,
        {
            unsafe { self.transmute_unchecked(cast) }
        }

        /// Casts to a different (unsized) target type.
        ///
        /// # Safety
        ///
        /// The caller promises that `u = cast(p)` is a pointer cast with the
        /// following properties:
        /// - `u` addresses a subset of the bytes addressed by `p`
        /// - `u` has the same provenance as `p`
        #[doc(hidden)]
        #[inline]
        pub unsafe fn cast_unsized<U, F, R>(
            self,
            cast: F,
        ) -> Ptr<'a, U, (I::Aliasing, Unaligned, I::Validity)>
        where
            T: MutationCompatible<U, I::Aliasing, I::Validity, I::Validity, R>,
            U: 'a + ?Sized + CastableFrom<T, I::Validity, I::Validity>,
            F: FnOnce(PtrInner<'a, T>) -> PtrInner<'a, U>,
        {
            unsafe { self.cast_unsized_unchecked(cast) }
        }
    }

    impl<'a, T, I> Ptr<'a, T, I>
    where
        T: 'a + KnownLayout + ?Sized,
        I: Invariants<Validity = Initialized>,
    {
        /// Casts this pointer-to-initialized into a pointer-to-bytes.
        #[allow(clippy::wrong_self_convention)]
        #[must_use]
        #[inline]
        pub fn as_bytes<R>(self) -> Ptr<'a, [u8], (I::Aliasing, Aligned, Valid)>
        where
            T: Read<I::Aliasing, R>,
            I::Aliasing: Reference,
        {
            let ptr = unsafe { self.cast_unsized(PtrInner::as_bytes) };
            ptr.bikeshed_recall_aligned().recall_validity::<Valid, (_, (_, _))>()
        }
    }

    impl<'a, T, I, const N: usize> Ptr<'a, [T; N], I>
    where
        T: 'a,
        I: Invariants,
    {
        /// Casts this pointer-to-array into a slice.
        #[allow(clippy::wrong_self_convention)]
        pub(crate) fn as_slice(self) -> Ptr<'a, [T], I> {
            let slice = self.as_inner().as_slice();
            unsafe { Ptr::from_inner(slice) }
        }
    }

    /// For caller convenience, these methods are generic over alignment
    /// invariant. In practice, the referent is always well-aligned, because the
    /// alignment of `[u8]` is 1.
    impl<'a, I> Ptr<'a, [u8], I>
    where
        I: Invariants<Validity = Valid>,
    {
        /// Attempts to cast `self` to a `U` using the given cast type.
        ///
        /// If `U` is a slice DST and pointer metadata (`meta`) is provided,
        /// then the cast will only succeed if it would produce an object with
        /// the given metadata.
        ///
        /// Returns `None` if the resulting `U` would be invalidly-aligned, if
        /// no `U` can fit in `self`, or if the provided pointer metadata
        /// describes an invalid instance of `U`. On success, returns a pointer
        /// to the largest-possible `U` which fits in `self`.
        ///
        /// # Safety
        ///
        /// The caller may assume that this implementation is correct, and may
        /// rely on that assumption for the soundness of their code. In
        /// particular, the caller may assume that, if `try_cast_into` returns
        /// `Some((ptr, remainder))`, then `ptr` and `remainder` refer to
        /// non-overlapping byte ranges within `self`, and that `ptr` and
        /// `remainder` entirely cover `self`. Finally:
        /// - If this is a prefix cast, `ptr` has the same address as `self`.
        /// - If this is a suffix cast, `remainder` has the same address as
        ///   `self`.
        #[inline(always)]
        pub(crate) fn try_cast_into<U, R>(
            self,
            cast_type: CastType,
            meta: Option<U::PointerMetadata>,
        ) -> Result<
            (Ptr<'a, U, (I::Aliasing, Aligned, Initialized)>, Ptr<'a, [u8], I>),
            CastError<Self, U>,
        >
        where
            I::Aliasing: Reference,
            U: 'a + ?Sized + KnownLayout + Read<I::Aliasing, R>,
        {
            let (inner, remainder) =
                self.as_inner().try_cast_into(cast_type, meta).map_err(|err| {
                    err.map_src(|inner|
                    unsafe { Ptr::from_inner(inner) })
                })?;

            let res = unsafe { Ptr::from_inner(inner) };

            let remainder = unsafe { Ptr::from_inner(remainder) };

            Ok((res, remainder))
        }

        /// Attempts to cast `self` into a `U`, failing if all of the bytes of
        /// `self` cannot be treated as a `U`.
        ///
        /// In particular, this method fails if `self` is not validly-aligned
        /// for `U` or if `self`'s size is not a valid size for `U`.
        ///
        /// # Safety
        ///
        /// On success, the caller may assume that the returned pointer
        /// references the same byte range as `self`.
        #[allow(unused)]
        #[inline(always)]
        pub(crate) fn try_cast_into_no_leftover<U, R>(
            self,
            meta: Option<U::PointerMetadata>,
        ) -> Result<Ptr<'a, U, (I::Aliasing, Aligned, Initialized)>, CastError<Self, U>>
        where
            I::Aliasing: Reference,
            U: 'a + ?Sized + KnownLayout + Read<I::Aliasing, R>,
        {
            #[allow(unstable_name_collisions)]
            match self.try_cast_into(CastType::Prefix, meta) {
                Ok((slf, remainder)) => {
                    if remainder.len() == 0 {
                        Ok(slf)
                    } else {
                        let slf = slf.as_bytes();
                        let slf = unsafe { slf.assume_alignment::<I::Alignment>() };
                        let slf = slf.unify_invariants();
                        Err(CastError::Size(SizeError::<_, U>::new(slf)))
                    }
                }
                Err(err) => Err(err),
            }
        }
    }

    impl<'a, T, I> Ptr<'a, core::cell::UnsafeCell<T>, I>
    where
        T: 'a + ?Sized,
        I: Invariants<Aliasing = Exclusive>,
    {
        /// Converts this `Ptr` into a pointer to the underlying data.
        ///
        /// This call borrows the `UnsafeCell` mutably (at compile-time) which
        /// guarantees that we possess the only reference.
        ///
        /// This is like [`UnsafeCell::get_mut`], but for `Ptr`.
        ///
        /// [`UnsafeCell::get_mut`]: core::cell::UnsafeCell::get_mut
        #[must_use]
        #[inline(always)]
        pub fn get_mut(self) -> Ptr<'a, T, I> {
            #[allow(clippy::as_conversions)]
            let ptr = unsafe { self.transmute_unchecked(|ptr| cast!(ptr)) };

            let ptr = unsafe { ptr.assume_alignment::<I::Alignment>() };
            ptr.unify_invariants()
        }
    }
}

/// Projections through the referent.
mod _project {
    use super::*;

    impl<'a, T, I> Ptr<'a, [T], I>
    where
        T: 'a,
        I: Invariants,
        I::Aliasing: Reference,
    {
        /// Iteratively projects the elements `Ptr<T>` from `Ptr<[T]>`.
        pub(crate) fn iter(&self) -> impl Iterator<Item = Ptr<'a, T, I>> {
            self.as_inner().iter().map(|elem| unsafe { Ptr::from_inner(elem) })
        }
    }

    #[allow(clippy::needless_lifetimes)]
    impl<'a, T, I> Ptr<'a, T, I>
    where
        T: 'a + ?Sized + KnownLayout<PointerMetadata = usize>,
        I: Invariants,
    {
        /// The number of slice elements in the object referenced by `self`.
        pub(crate) fn len(&self) -> usize {
            self.as_inner().meta().get()
        }
    }
}
