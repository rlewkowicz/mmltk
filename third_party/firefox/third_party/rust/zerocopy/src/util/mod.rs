// Copyright 2023 The Fuchsia Authors
// Licensed under a BSD-style license <LICENSE-BSD>, Apache License, Version 2.0
// <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
// license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.

#[macro_use]
pub(crate) mod macros;

#[doc(hidden)]
pub mod macro_util;

use core::{
    marker::PhantomData,
    mem::{self, ManuallyDrop},
    num::NonZeroUsize,
    ptr::NonNull,
};

use super::*;

/// Like [`PhantomData`], but [`Send`] and [`Sync`] regardless of whether the
/// wrapped `T` is.
pub(crate) struct SendSyncPhantomData<T: ?Sized>(PhantomData<T>);

unsafe impl<T: ?Sized> Send for SendSyncPhantomData<T> {}
unsafe impl<T: ?Sized> Sync for SendSyncPhantomData<T> {}

impl<T: ?Sized> Default for SendSyncPhantomData<T> {
    fn default() -> SendSyncPhantomData<T> {
        SendSyncPhantomData(PhantomData)
    }
}

impl<T: ?Sized> PartialEq for SendSyncPhantomData<T> {
    fn eq(&self, other: &Self) -> bool {
        self.0.eq(&other.0)
    }
}

impl<T: ?Sized> Eq for SendSyncPhantomData<T> {}

pub(crate) trait AsAddress {
    fn addr(self) -> usize;
}

impl<T: ?Sized> AsAddress for &T {
    #[inline(always)]
    fn addr(self) -> usize {
        let ptr: *const T = self;
        AsAddress::addr(ptr)
    }
}

impl<T: ?Sized> AsAddress for &mut T {
    #[inline(always)]
    fn addr(self) -> usize {
        let ptr: *const T = self;
        AsAddress::addr(ptr)
    }
}

impl<T: ?Sized> AsAddress for NonNull<T> {
    #[inline(always)]
    fn addr(self) -> usize {
        AsAddress::addr(self.as_ptr())
    }
}

impl<T: ?Sized> AsAddress for *const T {
    #[inline(always)]
    fn addr(self) -> usize {
        #[allow(clippy::as_conversions)]
#[cfg_attr(__ZEROCOPY_INTERNAL_USE_ONLY_NIGHTLY_FEATURES_IN_TESTS, allow(lossy_provenance_casts))]
return self.cast::<()>() as usize;
    }
}

impl<T: ?Sized> AsAddress for *mut T {
    #[inline(always)]
    fn addr(self) -> usize {
        let ptr: *const T = self;
        AsAddress::addr(ptr)
    }
}

/// Validates that `t` is aligned to `align_of::<U>()`.
#[inline(always)]
pub(crate) fn validate_aligned_to<T: AsAddress, U>(t: T) -> Result<(), AlignmentError<(), U>> {
    #[allow(clippy::arithmetic_side_effects)]
    let remainder = t.addr() % mem::align_of::<U>();
    if remainder == 0 {
        Ok(())
    } else {
        Err(unsafe { AlignmentError::new_unchecked(()) })
    }
}

/// Returns the bytes needed to pad `len` to the next multiple of `align`.
///
/// This function assumes that align is a power of two; there are no guarantees
/// on the answer it gives if this is not the case.
#[cfg_attr(kani, kani::requires(len <= isize::MAX as usize),
    kani::requires(align.is_power_of_two()),
    kani::ensures(|&p| (len + p) % align.get() == 0),
    kani::ensures(|&p| p < align.get()),)]
pub(crate) const fn padding_needed_for(len: usize, align: NonZeroUsize) -> usize {
    #[cfg(kani)]
    #[kani::proof_for_contract(padding_needed_for)]
    fn proof() {
        padding_needed_for(kani::any(), kani::any());
    }

    #[allow(clippy::arithmetic_side_effects)]
    let mask = align.get() - 1;

    !(len.wrapping_sub(1)) & mask
}

/// Rounds `n` down to the largest value `m` such that `m <= n` and `m % align
/// == 0`.
///
/// # Panics
///
/// May panic if `align` is not a power of two. Even if it doesn't panic in this
/// case, it will produce nonsense results.
#[inline(always)]
#[cfg_attr(kani, kani::requires(align.is_power_of_two()),
    kani::ensures(|&m| m <= n && m % align.get() == 0),
    kani::ensures(|&m| {
        m.checked_add(align.get()).map(|next_mul| next_mul > n).unwrap_or(true)
    }))]
pub(crate) const fn round_down_to_next_multiple_of_alignment(
    n: usize,
    align: NonZeroUsize,
) -> usize {
    #[cfg(kani)]
    #[kani::proof_for_contract(round_down_to_next_multiple_of_alignment)]
    fn proof() {
        round_down_to_next_multiple_of_alignment(kani::any(), kani::any());
    }

    let align = align.get();
    #[cfg(zerocopy_panic_in_const_and_vec_try_reserve_1_57_0)]
    debug_assert!(align.is_power_of_two());

    #[allow(clippy::arithmetic_side_effects)]
    let mask = !(align - 1);
    n & mask
}

pub(crate) const fn max(a: NonZeroUsize, b: NonZeroUsize) -> NonZeroUsize {
    if a.get() < b.get() {
        b
    } else {
        a
    }
}

pub(crate) const fn min(a: NonZeroUsize, b: NonZeroUsize) -> NonZeroUsize {
    if a.get() > b.get() {
        b
    } else {
        a
    }
}

/// Copies `src` into the prefix of `dst`.
///
/// # Safety
///
/// The caller guarantees that `src.len() <= dst.len()`.
#[inline(always)]
pub(crate) unsafe fn copy_unchecked(src: &[u8], dst: &mut [u8]) {
    debug_assert!(src.len() <= dst.len());
    unsafe {
        core::ptr::copy_nonoverlapping(src.as_ptr(), dst.as_mut_ptr(), src.len());
    };
}

/// Unsafely transmutes the given `src` into a type `Dst`.
///
/// # Safety
///
/// The value `src` must be a valid instance of `Dst`.
#[inline(always)]
pub(crate) const unsafe fn transmute_unchecked<Src, Dst>(src: Src) -> Dst {
    static_assert!(Src, Dst => core::mem::size_of::<Src>() == core::mem::size_of::<Dst>());

    #[repr(C)]
    union Transmute<Src, Dst> {
        src: ManuallyDrop<Src>,
        dst: ManuallyDrop<Dst>,
    }

    unsafe { ManuallyDrop::into_inner(Transmute { src: ManuallyDrop::new(src) }.dst) }
}

/// Uses `allocate` to create a `Box<T>`.
///
/// # Errors
///
/// Returns an error on allocation failure. Allocation failure is guaranteed
/// never to cause a panic or an abort.
///
/// # Safety
///
/// `allocate` must be either `alloc::alloc::alloc` or
/// `alloc::alloc::alloc_zeroed`. The referent of the box returned by `new_box`
/// has the same bit-validity as the referent of the pointer returned by the
/// given `allocate` and sufficient size to store `T` with `meta`.
#[must_use = "has no side effects (other than allocation)"]
#[cfg(feature = "alloc")]
#[inline]
pub(crate) unsafe fn new_box<T>(
    meta: T::PointerMetadata,
    allocate: unsafe fn(core::alloc::Layout) -> *mut u8,
) -> Result<alloc::boxed::Box<T>, AllocError>
where
    T: ?Sized + crate::KnownLayout,
{
    let size = match T::size_for_metadata(meta) {
        Some(size) => size,
        None => return Err(AllocError),
    };

    let align = T::LAYOUT.align.get();
    #[allow(clippy::as_conversions)]
    let max_alloc = (isize::MAX as usize).saturating_sub(align);
    if size > max_alloc {
        return Err(AllocError);
    }

    let layout = Layout::from_size_align(size, align).or(Err(AllocError))?;

    let ptr = if layout.size() != 0 {
        let ptr = unsafe { allocate(layout) };
        match NonNull::new(ptr) {
            Some(ptr) => ptr,
            None => return Err(AllocError),
        }
    } else {
        let align = T::LAYOUT.align.get();
        #[allow(unknown_lints)] 
        #[allow(clippy::useless_transmute, integer_to_ptr_transmutes)]
        let dangling = unsafe { mem::transmute::<usize, *mut u8>(align) };
        unsafe { NonNull::new_unchecked(dangling) }
    };

    let ptr = T::raw_from_ptr_len(ptr, meta);

    #[allow(clippy::undocumented_unsafe_blocks)]
    Ok(unsafe { alloc::boxed::Box::from_raw(ptr.as_ptr()) })
}

mod len_of {
    use super::*;

    /// A witness type for metadata of a valid instance of `&T`.
    pub(crate) struct MetadataOf<T: ?Sized + KnownLayout> {
        /// # Safety
        ///
        /// The size of an instance of `&T` with the given metadata is not
        /// larger than `isize::MAX`.
        meta: T::PointerMetadata,
        _p: PhantomData<T>,
    }

    impl<T: ?Sized + KnownLayout> Copy for MetadataOf<T> {}
    impl<T: ?Sized + KnownLayout> Clone for MetadataOf<T> {
        fn clone(&self) -> Self {
            *self
        }
    }

    impl<T: ?Sized> MetadataOf<T>
    where
        T: KnownLayout,
    {
        /// Returns `None` if `meta` is greater than `t`'s metadata.
        #[inline(always)]
        pub(crate) fn new_in_bounds(t: &T, meta: usize) -> Option<Self>
        where
            T: KnownLayout<PointerMetadata = usize>,
        {
            if meta <= Ptr::from_ref(t).len() {
                Some(unsafe { Self::new_unchecked(meta) })
            } else {
                None
            }
        }

        /// # Safety
        ///
        /// The size of an instance of `&T` with the given metadata is not
        /// larger than `isize::MAX`.
        pub(crate) unsafe fn new_unchecked(meta: T::PointerMetadata) -> Self {
            Self { meta, _p: PhantomData }
        }

        pub(crate) fn get(&self) -> T::PointerMetadata
        where
            T::PointerMetadata: Copy,
        {
            self.meta
        }

        #[inline]
        pub(crate) fn padding_needed_for(&self) -> usize
        where
            T: KnownLayout<PointerMetadata = usize>,
        {
            let trailing_slice_layout = crate::trailing_slice_layout::<T>();
            #[allow(unstable_name_collisions, clippy::incompatible_msrv)]
            let unpadded_size = unsafe {
                let trailing_size = self.meta.unchecked_mul(trailing_slice_layout.elem_size);
                trailing_size.unchecked_add(trailing_slice_layout.offset)
            };

            util::padding_needed_for(unpadded_size, T::LAYOUT.align)
        }

        #[inline(always)]
        pub(crate) fn validate_cast_and_convert_metadata(
            addr: usize,
            bytes_len: MetadataOf<[u8]>,
            cast_type: CastType,
            meta: Option<T::PointerMetadata>,
        ) -> Result<(MetadataOf<T>, MetadataOf<[u8]>), MetadataCastError> {
            let layout = match meta {
                None => T::LAYOUT,
                Some(meta) => {
                    let size = match T::size_for_metadata(meta) {
                        Some(size) => size,
                        None => return Err(MetadataCastError::Size),
                    };
                    DstLayout {
                        align: T::LAYOUT.align,
                        size_info: crate::SizeInfo::Sized { size },
                        statically_shallow_unpadded: false,
                    }
                }
            };
            let (elems, split_at) =
                layout.validate_cast_and_convert_metadata(addr, bytes_len.get(), cast_type)?;
            let elems = T::PointerMetadata::from_elem_count(elems);

            let elems = meta.unwrap_or(elems);

            let elems = unsafe { MetadataOf::new_unchecked(elems) };

            let split_at = unsafe { MetadataOf::<[u8]>::new_unchecked(split_at) };
            Ok((elems, split_at))
        }
    }
}

pub(crate) use len_of::MetadataOf;

/// Since we support multiple versions of Rust, there are often features which
/// have been stabilized in the most recent stable release which do not yet
/// exist (stably) on our MSRV. This module provides polyfills for those
/// features so that we can write more "modern" code, and just remove the
/// polyfill once our MSRV supports the corresponding feature. Without this,
/// we'd have to write worse/more verbose code and leave FIXME comments sprinkled
/// throughout the codebase to update to the new pattern once it's stabilized.
///
/// Each trait is imported as `_` at the crate root; each polyfill should "just
/// work" at usage sites.
pub(crate) mod polyfills {
    use core::ptr::{self, NonNull};

    #[allow(unused)]
    pub(crate) trait NonNullExt<T> {
        fn slice_from_raw_parts(data: Self, len: usize) -> NonNull<[T]>;
    }

    impl<T> NonNullExt<T> for NonNull<T> {
#[cfg_attr(all(coverage_nightly, __ZEROCOPY_INTERNAL_USE_ONLY_NIGHTLY_FEATURES_IN_TESTS), coverage(off))]
#[inline(always)]
fn slice_from_raw_parts(data: Self, len: usize) -> NonNull<[T]> {
            let ptr = ptr::slice_from_raw_parts_mut(data.as_ptr(), len);
            unsafe { NonNull::new_unchecked(ptr) }
        }
    }

    #[allow(unused)]
    pub(crate) trait NumExt {
        /// Add without checking for overflow.
        ///
        /// # Safety
        ///
        /// The caller promises that the addition will not overflow.
        unsafe fn unchecked_add(self, rhs: Self) -> Self;

        /// Subtract without checking for underflow.
        ///
        /// # Safety
        ///
        /// The caller promises that the subtraction will not underflow.
        unsafe fn unchecked_sub(self, rhs: Self) -> Self;

        /// Multiply without checking for overflow.
        ///
        /// # Safety
        ///
        /// The caller promises that the multiplication will not overflow.
        unsafe fn unchecked_mul(self, rhs: Self) -> Self;
    }

    impl NumExt for usize {
#[cfg_attr(all(coverage_nightly, __ZEROCOPY_INTERNAL_USE_ONLY_NIGHTLY_FEATURES_IN_TESTS), coverage(off))]
#[inline(always)]
unsafe fn unchecked_add(self, rhs: usize) -> usize {
            match self.checked_add(rhs) {
                Some(x) => x,
                None => {
                    unsafe { core::hint::unreachable_unchecked() }
                }
            }
        }

#[cfg_attr(all(coverage_nightly, __ZEROCOPY_INTERNAL_USE_ONLY_NIGHTLY_FEATURES_IN_TESTS), coverage(off))]
#[inline(always)]
unsafe fn unchecked_sub(self, rhs: usize) -> usize {
            match self.checked_sub(rhs) {
                Some(x) => x,
                None => {
                    unsafe { core::hint::unreachable_unchecked() }
                }
            }
        }

#[cfg_attr(all(coverage_nightly, __ZEROCOPY_INTERNAL_USE_ONLY_NIGHTLY_FEATURES_IN_TESTS), coverage(off))]
#[inline(always)]
unsafe fn unchecked_mul(self, rhs: usize) -> usize {
            match self.checked_mul(rhs) {
                Some(x) => x,
                None => {
                    unsafe { core::hint::unreachable_unchecked() }
                }
            }
        }
    }
}
