// Copyright 2023 The Fuchsia Authors
// Licensed under a BSD-style license <LICENSE-BSD>, Apache License, Version 2.0
// <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
// license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.

use core::{fmt, hash::Hash};

use super::*;

/// A type with no alignment requirement.
///
/// An `Unalign` wraps a `T`, removing any alignment requirement. `Unalign<T>`
/// has the same size and bit validity as `T`, but not necessarily the same
/// alignment [or ABI]. This is useful if a type with an alignment requirement
/// needs to be read from a chunk of memory which provides no alignment
/// guarantees.
///
/// Since `Unalign` has no alignment requirement, the inner `T` may not be
/// properly aligned in memory. There are five ways to access the inner `T`:
/// - by value, using [`get`] or [`into_inner`]
/// - by reference inside of a callback, using [`update`]
/// - fallibly by reference, using [`try_deref`] or [`try_deref_mut`]; these can
///   fail if the `Unalign` does not satisfy `T`'s alignment requirement at
///   runtime
/// - unsafely by reference, using [`deref_unchecked`] or
///   [`deref_mut_unchecked`]; it is the caller's responsibility to ensure that
///   the `Unalign` satisfies `T`'s alignment requirement
/// - (where `T: Unaligned`) infallibly by reference, using [`Deref::deref`] or
///   [`DerefMut::deref_mut`]
///
/// [or ABI]: https://github.com/google/zerocopy/issues/164
/// [`get`]: Unalign::get
/// [`into_inner`]: Unalign::into_inner
/// [`update`]: Unalign::update
/// [`try_deref`]: Unalign::try_deref
/// [`try_deref_mut`]: Unalign::try_deref_mut
/// [`deref_unchecked`]: Unalign::deref_unchecked
/// [`deref_mut_unchecked`]: Unalign::deref_mut_unchecked
///
/// # Example
///
/// In this example, we need `EthernetFrame` to have no alignment requirement -
/// and thus implement [`Unaligned`]. `EtherType` is `#[repr(u16)]` and so
/// cannot implement `Unaligned`. We use `Unalign` to relax `EtherType`'s
/// alignment requirement so that `EthernetFrame` has no alignment requirement
/// and can implement `Unaligned`.
///
/// ```rust
/// use zerocopy::*;
/// # use zerocopy_derive::*;
/// # #[derive(FromBytes, KnownLayout, Immutable, Unaligned)] #[repr(C)] struct Mac([u8; 6]);
///
/// # #[derive(PartialEq, Copy, Clone, Debug)]
/// #[derive(TryFromBytes, KnownLayout, Immutable)]
/// #[repr(u16)]
/// enum EtherType {
///     Ipv4 = 0x0800u16.to_be(),
///     Arp = 0x0806u16.to_be(),
///     Ipv6 = 0x86DDu16.to_be(),
///     # /*
///     ...
///     # */
/// }
///
/// #[derive(TryFromBytes, KnownLayout, Immutable, Unaligned)]
/// #[repr(C)]
/// struct EthernetFrame {
///     src: Mac,
///     dst: Mac,
///     ethertype: Unalign<EtherType>,
///     payload: [u8],
/// }
///
/// let bytes = &[
///     # 0, 1, 2, 3, 4, 5,
///     # 6, 7, 8, 9, 10, 11,
///     # /*
///     ...
///     # */
///     0x86, 0xDD,            // EtherType
///     0xDE, 0xAD, 0xBE, 0xEF // Payload
/// ][..];
///
/// // PANICS: Guaranteed not to panic because `bytes` is of the right
/// // length, has the right contents, and `EthernetFrame` has no
/// // alignment requirement.
/// let packet = EthernetFrame::try_ref_from_bytes(&bytes).unwrap();
///
/// assert_eq!(packet.ethertype.get(), EtherType::Ipv6);
/// assert_eq!(packet.payload, [0xDE, 0xAD, 0xBE, 0xEF]);
/// ```
///
/// # Safety
///
/// `Unalign<T>` is guaranteed to have the same size and bit validity as `T`,
/// and to have [`UnsafeCell`]s covering the same byte ranges as `T`.
/// `Unalign<T>` is guaranteed to have alignment 1.
#[allow(missing_debug_implementations)]
#[derive(Default, Copy)]
#[cfg_attr(feature = "derive", derive(Immutable, FromBytes, IntoBytes, Unaligned))]
#[repr(C, packed)]
pub struct Unalign<T>(T);

impl_known_layout!(T => Unalign<T>);

#[allow(unused_unsafe)] 
const _: () = unsafe {
    impl_or_verify!(T => Unaligned for Unalign<T>);
    impl_or_verify!(T: Immutable => Immutable for Unalign<T>);
    impl_or_verify!(
        T: TryFromBytes => TryFromBytes for Unalign<T>;
        |c| T::is_bit_valid(c.transmute())
    );
    impl_or_verify!(T: FromZeros => FromZeros for Unalign<T>);
    impl_or_verify!(T: FromBytes => FromBytes for Unalign<T>);
    impl_or_verify!(T: IntoBytes => IntoBytes for Unalign<T>);
};

impl<T: Copy> Clone for Unalign<T> {
    #[inline(always)]
    fn clone(&self) -> Unalign<T> {
        *self
    }
}

impl<T> Unalign<T> {
    /// Constructs a new `Unalign`.
    #[inline(always)]
    pub const fn new(val: T) -> Unalign<T> {
        Unalign(val)
    }

    /// Consumes `self`, returning the inner `T`.
    #[inline(always)]
    pub const fn into_inner(self) -> T {
        unsafe { crate::util::transmute_unchecked(self) }
    }

    /// Attempts to return a reference to the wrapped `T`, failing if `self` is
    /// not properly aligned.
    ///
    /// If `self` does not satisfy `align_of::<T>()`, then `try_deref` returns
    /// `Err`.
    ///
    /// If `T: Unaligned`, then `Unalign<T>` implements [`Deref`], and callers
    /// may prefer [`Deref::deref`], which is infallible.
    #[inline(always)]
    pub fn try_deref(&self) -> Result<&T, AlignmentError<&Self, T>> {
        let inner = Ptr::from_ref(self).transmute();
        match inner.try_into_aligned() {
            Ok(aligned) => Ok(aligned.as_ref()),
            Err(err) => Err(err.map_src(|src| src.into_unalign().as_ref())),
        }
    }

    /// Attempts to return a mutable reference to the wrapped `T`, failing if
    /// `self` is not properly aligned.
    ///
    /// If `self` does not satisfy `align_of::<T>()`, then `try_deref` returns
    /// `Err`.
    ///
    /// If `T: Unaligned`, then `Unalign<T>` implements [`DerefMut`], and
    /// callers may prefer [`DerefMut::deref_mut`], which is infallible.
    #[inline(always)]
    pub fn try_deref_mut(&mut self) -> Result<&mut T, AlignmentError<&mut Self, T>> {
        let inner = Ptr::from_mut(self).transmute::<_, _, (_, (_, _))>();
        match inner.try_into_aligned() {
            Ok(aligned) => Ok(aligned.as_mut()),
            Err(err) => Err(err.map_src(|src| src.into_unalign().as_mut())),
        }
    }

    /// Returns a reference to the wrapped `T` without checking alignment.
    ///
    /// If `T: Unaligned`, then `Unalign<T>` implements[ `Deref`], and callers
    /// may prefer [`Deref::deref`], which is safe.
    ///
    /// # Safety
    ///
    /// The caller must guarantee that `self` satisfies `align_of::<T>()`.
    #[inline(always)]
    pub const unsafe fn deref_unchecked(&self) -> &T {
        unsafe { mem::transmute(self) }
    }

    /// Returns a mutable reference to the wrapped `T` without checking
    /// alignment.
    ///
    /// If `T: Unaligned`, then `Unalign<T>` implements[ `DerefMut`], and
    /// callers may prefer [`DerefMut::deref_mut`], which is safe.
    ///
    /// # Safety
    ///
    /// The caller must guarantee that `self` satisfies `align_of::<T>()`.
    #[inline(always)]
    pub unsafe fn deref_mut_unchecked(&mut self) -> &mut T {
        unsafe { &mut *self.get_mut_ptr() }
    }

    /// Gets an unaligned raw pointer to the inner `T`.
    ///
    /// # Safety
    ///
    /// The returned raw pointer is not necessarily aligned to
    /// `align_of::<T>()`. Most functions which operate on raw pointers require
    /// those pointers to be aligned, so calling those functions with the result
    /// of `get_ptr` will result in undefined behavior if alignment is not
    /// guaranteed using some out-of-band mechanism. In general, the only
    /// functions which are safe to call with this pointer are those which are
    /// explicitly documented as being sound to use with an unaligned pointer,
    /// such as [`read_unaligned`].
    ///
    /// Even if the caller is permitted to mutate `self` (e.g. they have
    /// ownership or a mutable borrow), it is not guaranteed to be sound to
    /// write through the returned pointer. If writing is required, prefer
    /// [`get_mut_ptr`] instead.
    ///
    /// [`read_unaligned`]: core::ptr::read_unaligned
    /// [`get_mut_ptr`]: Unalign::get_mut_ptr
    #[inline(always)]
    pub const fn get_ptr(&self) -> *const T {
        ptr::addr_of!(self.0)
    }

    /// Gets an unaligned mutable raw pointer to the inner `T`.
    ///
    /// # Safety
    ///
    /// The returned raw pointer is not necessarily aligned to
    /// `align_of::<T>()`. Most functions which operate on raw pointers require
    /// those pointers to be aligned, so calling those functions with the result
    /// of `get_ptr` will result in undefined behavior if alignment is not
    /// guaranteed using some out-of-band mechanism. In general, the only
    /// functions which are safe to call with this pointer are those which are
    /// explicitly documented as being sound to use with an unaligned pointer,
    /// such as [`read_unaligned`].
    ///
    /// [`read_unaligned`]: core::ptr::read_unaligned
    #[inline(always)]
    pub fn get_mut_ptr(&mut self) -> *mut T {
        ptr::addr_of_mut!(self.0)
    }

    /// Sets the inner `T`, dropping the previous value.
    #[inline(always)]
    pub fn set(&mut self, t: T) {
        *self = Unalign::new(t);
    }

    /// Updates the inner `T` by calling a function on it.
    ///
    /// If [`T: Unaligned`], then `Unalign<T>` implements [`DerefMut`], and that
    /// impl should be preferred over this method when performing updates, as it
    /// will usually be faster and more ergonomic.
    ///
    /// For large types, this method may be expensive, as it requires copying
    /// `2 * size_of::<T>()` bytes. \[1\]
    ///
    /// \[1\] Since the inner `T` may not be aligned, it would not be sound to
    /// invoke `f` on it directly. Instead, `update` moves it into a
    /// properly-aligned location in the local stack frame, calls `f` on it, and
    /// then moves it back to its original location in `self`.
    ///
    /// [`T: Unaligned`]: Unaligned
    #[inline]
    pub fn update<O, F: FnOnce(&mut T) -> O>(&mut self, f: F) -> O {
        if mem::align_of::<T>() == 1 {

            let t = unsafe { self.deref_mut_unchecked() };
            return f(t);
        }

        struct WriteBackOnDrop<T> {
            copy: ManuallyDrop<T>,
            slf: *mut Unalign<T>,
        }

        impl<T> Drop for WriteBackOnDrop<T> {
            fn drop(&mut self) {
                let copy = unsafe { ManuallyDrop::take(&mut self.copy) };
                unsafe { ptr::write(self.slf, Unalign::new(copy)) };
            }
        }

        let copy = unsafe { ptr::read(self) }.into_inner();
        let mut write_back = WriteBackOnDrop { copy: ManuallyDrop::new(copy), slf: self };

        let ret = f(&mut write_back.copy);

        drop(write_back);
        ret
    }
}

impl<T: Copy> Unalign<T> {
    /// Gets a copy of the inner `T`.
    #[inline(always)]
    pub fn get(&self) -> T {
        let Unalign(val) = *self;
        val
    }
}

impl<T: Unaligned> Deref for Unalign<T> {
    type Target = T;

    #[inline(always)]
    fn deref(&self) -> &T {
        Ptr::from_ref(self).transmute().bikeshed_recall_aligned().as_ref()
    }
}

impl<T: Unaligned> DerefMut for Unalign<T> {
    #[inline(always)]
    fn deref_mut(&mut self) -> &mut T {
        Ptr::from_mut(self).transmute::<_, _, (_, (_, _))>().bikeshed_recall_aligned().as_mut()
    }
}

impl<T: Unaligned + PartialOrd> PartialOrd<Unalign<T>> for Unalign<T> {
    #[inline(always)]
    fn partial_cmp(&self, other: &Unalign<T>) -> Option<Ordering> {
        PartialOrd::partial_cmp(self.deref(), other.deref())
    }
}

impl<T: Unaligned + Ord> Ord for Unalign<T> {
    #[inline(always)]
    fn cmp(&self, other: &Unalign<T>) -> Ordering {
        Ord::cmp(self.deref(), other.deref())
    }
}

impl<T: Unaligned + PartialEq> PartialEq<Unalign<T>> for Unalign<T> {
    #[inline(always)]
    fn eq(&self, other: &Unalign<T>) -> bool {
        PartialEq::eq(self.deref(), other.deref())
    }
}

impl<T: Unaligned + Eq> Eq for Unalign<T> {}

impl<T: Unaligned + Hash> Hash for Unalign<T> {
    #[inline(always)]
    fn hash<H>(&self, state: &mut H)
    where
        H: Hasher,
    {
        self.deref().hash(state);
    }
}

impl<T: Unaligned + Debug> Debug for Unalign<T> {
    #[inline(always)]
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        Debug::fmt(self.deref(), f)
    }
}

impl<T: Unaligned + Display> Display for Unalign<T> {
    #[inline(always)]
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        Display::fmt(self.deref(), f)
    }
}

/// A wrapper type to construct uninitialized instances of `T`.
///
/// `MaybeUninit` is identical to the [standard library
/// `MaybeUninit`][core-maybe-uninit] type except that it supports unsized
/// types.
///
/// # Layout
///
/// The same layout guarantees and caveats apply to `MaybeUninit<T>` as apply to
/// the [standard library `MaybeUninit`][core-maybe-uninit] with one exception:
/// for `T: !Sized`, there is no single value for `T`'s size. Instead, for such
/// types, the following are guaranteed:
/// - Every [valid size][valid-size] for `T` is a valid size for
///   `MaybeUninit<T>` and vice versa
/// - Given `t: *const T` and `m: *const MaybeUninit<T>` with identical fat
///   pointer metadata, `t` and `m` address the same number of bytes (and
///   likewise for `*mut`)
///
/// [core-maybe-uninit]: core::mem::MaybeUninit
/// [valid-size]: crate::KnownLayout#what-is-a-valid-size
#[repr(transparent)]
#[doc(hidden)]
pub struct MaybeUninit<T: ?Sized + KnownLayout>(
    T::MaybeUninit,
);

#[doc(hidden)]
impl<T: ?Sized + KnownLayout> MaybeUninit<T> {
    /// Constructs a `MaybeUninit<T>` initialized with the given value.
    #[inline(always)]
    pub fn new(val: T) -> Self
    where
        T: Sized,
        Self: Sized,
    {
        unsafe { crate::util::transmute_unchecked(val) }
    }

    /// Constructs an uninitialized `MaybeUninit<T>`.
    #[must_use]
    #[inline(always)]
    pub fn uninit() -> Self
    where
        T: Sized,
        Self: Sized,
    {
        let uninit = CoreMaybeUninit::<T>::uninit();
        unsafe { crate::util::transmute_unchecked(uninit) }
    }

    /// Creates a `Box<MaybeUninit<T>>`.
    ///
    /// This function is useful for allocating large, uninit values on the heap
    /// without ever creating a temporary instance of `Self` on the stack.
    ///
    /// # Errors
    ///
    /// Returns an error on allocation failure. Allocation failure is guaranteed
    /// never to cause a panic or an abort.
    #[cfg(feature = "alloc")]
    #[inline]
    pub fn new_boxed_uninit(meta: T::PointerMetadata) -> Result<Box<Self>, AllocError> {
        unsafe { crate::util::new_box(meta, alloc::alloc::alloc) }
    }

    /// Extracts the value from the `MaybeUninit<T>` container.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `self` is in an bit-valid state. Depending
    /// on subsequent use, it may also need to be in a library-valid state.
    #[inline(always)]
    pub unsafe fn assume_init(self) -> T
    where
        T: Sized,
        Self: Sized,
    {
        unsafe { crate::util::transmute_unchecked(self) }
    }
}

impl<T: ?Sized + KnownLayout> fmt::Debug for MaybeUninit<T> {
    #[inline]
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.pad(core::any::type_name::<Self>())
    }
}
