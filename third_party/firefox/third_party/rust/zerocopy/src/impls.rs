// Copyright 2024 The Fuchsia Authors
// Licensed under the 2-Clause BSD License <LICENSE-BSD or
// https://opensource.org/license/bsd-2-clause>, Apache License, Version 2.0
// <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
// license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.

use core::{
    cell::{Cell, UnsafeCell},
    mem::MaybeUninit as CoreMaybeUninit,
    ptr::NonNull,
};

use super::*;

const _: () = unsafe {
    unsafe_impl!((): Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes, Unaligned);
    assert_unaligned!(());
};

const _: () = unsafe {
    unsafe_impl!(u8: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes, Unaligned);
    unsafe_impl!(i8: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes, Unaligned);
    assert_unaligned!(u8, i8);
    unsafe_impl!(u16: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(i16: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(u32: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(i32: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(u64: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(i64: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(u128: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(i128: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(usize: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(isize: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(f32: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(f64: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    #[cfg(feature = "float-nightly")]
    unsafe_impl!(#[cfg_attr(doc_cfg, doc(cfg(feature = "float-nightly")))] f16: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
    #[cfg(feature = "float-nightly")]
    unsafe_impl!(#[cfg_attr(doc_cfg, doc(cfg(feature = "float-nightly")))] f128: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes);
};

const _: () = unsafe { unsafe_impl!(bool: Immutable, FromZeros, IntoBytes, Unaligned) };
assert_unaligned!(bool);

const _: () = unsafe {
    unsafe_impl!(=> TryFromBytes for bool; |byte| {
        let byte = byte.transmute::<u8, invariant::Valid, _>();
        *byte.unaligned_as_ref() < 2
    })
};
impl_size_eq!(bool, u8);

const _: () = unsafe { unsafe_impl!(char: Immutable, FromZeros, IntoBytes) };

const _: () = unsafe {
    unsafe_impl!(=> TryFromBytes for char; |c| {
        let c = c.transmute::<Unalign<u32>, invariant::Valid, _>();
        let c = c.read_unaligned().into_inner();
        char::from_u32(c).is_some()
    });
};

impl_size_eq!(char, Unalign<u32>);

const _: () = unsafe { unsafe_impl!(str: Immutable, FromZeros, IntoBytes, Unaligned) };

const _: () = unsafe {
    unsafe_impl!(=> TryFromBytes for str; |c| {
        let c = c.transmute::<[u8], invariant::Valid, _>();
        let c = c.unaligned_as_ref();
        core::str::from_utf8(c).is_ok()
    })
};

impl_size_eq!(str, [u8]);

macro_rules! unsafe_impl_try_from_bytes_for_nonzero {
    ($($nonzero:ident[$prim:ty]),*) => {
        $(
            unsafe_impl!(=> TryFromBytes for $nonzero; |n| {
                impl_size_eq!($nonzero, Unalign<$prim>);

                let n = n.transmute::<Unalign<$prim>, invariant::Valid, _>();
                $nonzero::new(n.read_unaligned().into_inner()).is_some()
            });
        )*
    }
}

const _: () = unsafe {
    unsafe_impl!(NonZeroU8: Immutable, IntoBytes, Unaligned);
    unsafe_impl!(NonZeroI8: Immutable, IntoBytes, Unaligned);
    assert_unaligned!(NonZeroU8, NonZeroI8);
    unsafe_impl!(NonZeroU16: Immutable, IntoBytes);
    unsafe_impl!(NonZeroI16: Immutable, IntoBytes);
    unsafe_impl!(NonZeroU32: Immutable, IntoBytes);
    unsafe_impl!(NonZeroI32: Immutable, IntoBytes);
    unsafe_impl!(NonZeroU64: Immutable, IntoBytes);
    unsafe_impl!(NonZeroI64: Immutable, IntoBytes);
    unsafe_impl!(NonZeroU128: Immutable, IntoBytes);
    unsafe_impl!(NonZeroI128: Immutable, IntoBytes);
    unsafe_impl!(NonZeroUsize: Immutable, IntoBytes);
    unsafe_impl!(NonZeroIsize: Immutable, IntoBytes);
    unsafe_impl_try_from_bytes_for_nonzero!(
        NonZeroU8[u8],
        NonZeroI8[i8],
        NonZeroU16[u16],
        NonZeroI16[i16],
        NonZeroU32[u32],
        NonZeroI32[i32],
        NonZeroU64[u64],
        NonZeroI64[i64],
        NonZeroU128[u128],
        NonZeroI128[i128],
        NonZeroUsize[usize],
        NonZeroIsize[isize]
    );
};

const _: () = unsafe {
    unsafe_impl!(Option<NonZeroU8>: TryFromBytes, FromZeros, FromBytes, IntoBytes, Unaligned);
    unsafe_impl!(Option<NonZeroI8>: TryFromBytes, FromZeros, FromBytes, IntoBytes, Unaligned);
    assert_unaligned!(Option<NonZeroU8>, Option<NonZeroI8>);
    unsafe_impl!(Option<NonZeroU16>: TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(Option<NonZeroI16>: TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(Option<NonZeroU32>: TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(Option<NonZeroI32>: TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(Option<NonZeroU64>: TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(Option<NonZeroI64>: TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(Option<NonZeroU128>: TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(Option<NonZeroI128>: TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(Option<NonZeroUsize>: TryFromBytes, FromZeros, FromBytes, IntoBytes);
    unsafe_impl!(Option<NonZeroIsize>: TryFromBytes, FromZeros, FromBytes, IntoBytes);
};

#[cfg(feature = "alloc")]
const _: () = unsafe {
    unsafe_impl!(
        #[cfg_attr(doc_cfg, doc(cfg(feature = "alloc")))]
        T: Sized => Immutable for Box<T>
    )
};

const _: () = unsafe {
    #[cfg(feature = "alloc")]
    unsafe_impl!(
        #[cfg_attr(doc_cfg, doc(cfg(feature = "alloc")))]
        T => TryFromBytes for Option<Box<T>>; |c| pointer::is_zeroed(c)
    );
    #[cfg(feature = "alloc")]
    unsafe_impl!(
        #[cfg_attr(doc_cfg, doc(cfg(feature = "alloc")))]
        T => FromZeros for Option<Box<T>>
    );
    unsafe_impl!(
        T => TryFromBytes for Option<&'_ T>; |c| pointer::is_zeroed(c)
    );
    unsafe_impl!(T => FromZeros for Option<&'_ T>);
    unsafe_impl!(
            T => TryFromBytes for Option<&'_ mut T>; |c| pointer::is_zeroed(c)
    );
    unsafe_impl!(T => FromZeros for Option<&'_ mut T>);
    unsafe_impl!(
        T => TryFromBytes for Option<NonNull<T>>; |c| pointer::is_zeroed(c)
    );
    unsafe_impl!(T => FromZeros for Option<NonNull<T>>);
    unsafe_impl_for_power_set!(A, B, C, D, E, F, G, H, I, J, K, L -> M => FromZeros for opt_fn!(...));
    unsafe_impl_for_power_set!(
        A, B, C, D, E, F, G, H, I, J, K, L -> M => TryFromBytes for opt_fn!(...);
        |c| pointer::is_zeroed(c)
    );
    unsafe_impl_for_power_set!(A, B, C, D, E, F, G, H, I, J, K, L -> M => FromZeros for opt_unsafe_fn!(...));
    unsafe_impl_for_power_set!(
        A, B, C, D, E, F, G, H, I, J, K, L -> M => TryFromBytes for opt_unsafe_fn!(...);
        |c| pointer::is_zeroed(c)
    );
    unsafe_impl_for_power_set!(A, B, C, D, E, F, G, H, I, J, K, L -> M => FromZeros for opt_extern_c_fn!(...));
    unsafe_impl_for_power_set!(
        A, B, C, D, E, F, G, H, I, J, K, L -> M => TryFromBytes for opt_extern_c_fn!(...);
        |c| pointer::is_zeroed(c)
    );
    unsafe_impl_for_power_set!(A, B, C, D, E, F, G, H, I, J, K, L -> M => FromZeros for opt_unsafe_extern_c_fn!(...));
    unsafe_impl_for_power_set!(
        A, B, C, D, E, F, G, H, I, J, K, L -> M => TryFromBytes for opt_unsafe_extern_c_fn!(...);
        |c| pointer::is_zeroed(c)
    );
};

const _: () = unsafe {
    unsafe_impl_for_power_set!(A, B, C, D, E, F, G, H, I, J, K, L -> M => Immutable for opt_fn!(...));
    unsafe_impl_for_power_set!(A, B, C, D, E, F, G, H, I, J, K, L -> M => Immutable for opt_unsafe_fn!(...));
    unsafe_impl_for_power_set!(A, B, C, D, E, F, G, H, I, J, K, L -> M => Immutable for opt_extern_c_fn!(...));
    unsafe_impl_for_power_set!(A, B, C, D, E, F, G, H, I, J, K, L -> M => Immutable for opt_unsafe_extern_c_fn!(...));
};

#[cfg(all(zerocopy_target_has_atomics_1_60_0, any(target_has_atomic = "8", target_has_atomic = "16", target_has_atomic = "32", target_has_atomic = "64", target_has_atomic = "ptr")))]
#[cfg_attr(doc_cfg, doc(cfg(rust = "1.60.0")))]
mod atomics {
    use super::*;

    macro_rules! impl_traits_for_atomics {
        ($($atomics:ident [$primitives:ident]),* $(,)?) => {
            $(
                impl_known_layout!($atomics);
                impl_for_transmute_from!(=> TryFromBytes for $atomics [UnsafeCell<$primitives>]);
                impl_for_transmute_from!(=> FromZeros for $atomics [UnsafeCell<$primitives>]);
                impl_for_transmute_from!(=> FromBytes for $atomics [UnsafeCell<$primitives>]);
                impl_for_transmute_from!(=> IntoBytes for $atomics [UnsafeCell<$primitives>]);
            )*
        };
    }

    /// Implements `TransmuteFrom` for `$atomic`, `$prim`, and
    /// `UnsafeCell<$prim>`.
    ///
    /// # Safety
    ///
    /// `$atomic` must have the same size and bit validity as `$prim`.
    macro_rules! unsafe_impl_transmute_from_for_atomic {
        ($($($tyvar:ident)? => $atomic:ty [$prim:ty]),*) => {{
            crate::util::macros::__unsafe();

            use core::cell::UnsafeCell;
            use crate::pointer::{PtrInner, SizeEq, TransmuteFrom, invariant::Valid};

            $(
                unsafe impl<$($tyvar)?> TransmuteFrom<$atomic, Valid, Valid> for $prim {}
                unsafe impl<$($tyvar)?> TransmuteFrom<$prim, Valid, Valid> for $atomic {}

                unsafe impl<$($tyvar)?> SizeEq<$atomic> for $prim {
                    #[inline]
                    fn cast_from_raw(a: PtrInner<'_, $atomic>) -> PtrInner<'_, $prim> {
                        unsafe { cast!(a) }
                    }
                }
                unsafe impl<$($tyvar)?> SizeEq<$prim> for $atomic {
                    #[inline]
                    fn cast_from_raw(p: PtrInner<'_, $prim>) -> PtrInner<'_, $atomic> {
                        unsafe { cast!(p) }
                    }
                }
                unsafe impl<$($tyvar)?> SizeEq<$atomic> for UnsafeCell<$prim> {
                    #[inline]
                    fn cast_from_raw(a: PtrInner<'_, $atomic>) -> PtrInner<'_, UnsafeCell<$prim>> {
                        unsafe { cast!(a) }
                    }
                }
                unsafe impl<$($tyvar)?> SizeEq<UnsafeCell<$prim>> for $atomic {
                    #[inline]
                    fn cast_from_raw(p: PtrInner<'_, UnsafeCell<$prim>>) -> PtrInner<'_, $atomic> {
                        unsafe { cast!(p) }
                    }
                }

                unsafe impl<$($tyvar)?> TransmuteFrom<$atomic, Valid, Valid> for core::cell::UnsafeCell<$prim> {}
                unsafe impl<$($tyvar)?> TransmuteFrom<core::cell::UnsafeCell<$prim>, Valid, Valid> for $atomic {}
            )*
        }};
    }

    #[cfg(target_has_atomic = "8")]
    #[cfg_attr(doc_cfg, doc(cfg(target_has_atomic = "8")))]
    mod atomic_8 {
        use core::sync::atomic::{AtomicBool, AtomicI8, AtomicU8};

        use super::*;

        impl_traits_for_atomics!(AtomicU8[u8], AtomicI8[i8]);

        impl_known_layout!(AtomicBool);

        impl_for_transmute_from!(=> TryFromBytes for AtomicBool [UnsafeCell<bool>]);
        impl_for_transmute_from!(=> FromZeros for AtomicBool [UnsafeCell<bool>]);
        impl_for_transmute_from!(=> IntoBytes for AtomicBool [UnsafeCell<bool>]);

        const _: () = unsafe {
            unsafe_impl!(AtomicBool: Unaligned);
            unsafe_impl!(AtomicU8: Unaligned);
            unsafe_impl!(AtomicI8: Unaligned);
            assert_unaligned!(AtomicBool, AtomicU8, AtomicI8);
        };

        const _: () = unsafe {
            unsafe_impl_transmute_from_for_atomic!(
                => AtomicU8 [u8],
                => AtomicI8 [i8],
                => AtomicBool [bool]
            )
        };
    }

    #[cfg(target_has_atomic = "16")]
    #[cfg_attr(doc_cfg, doc(cfg(target_has_atomic = "16")))]
    mod atomic_16 {
        use core::sync::atomic::{AtomicI16, AtomicU16};

        use super::*;

        impl_traits_for_atomics!(AtomicU16[u16], AtomicI16[i16]);

        const _: () = unsafe {
            unsafe_impl_transmute_from_for_atomic!(=> AtomicU16 [u16], => AtomicI16 [i16])
        };
    }

    #[cfg(target_has_atomic = "32")]
    #[cfg_attr(doc_cfg, doc(cfg(target_has_atomic = "32")))]
    mod atomic_32 {
        use core::sync::atomic::{AtomicI32, AtomicU32};

        use super::*;

        impl_traits_for_atomics!(AtomicU32[u32], AtomicI32[i32]);

        const _: () = unsafe {
            unsafe_impl_transmute_from_for_atomic!(=> AtomicU32 [u32], => AtomicI32 [i32])
        };
    }

    #[cfg(target_has_atomic = "64")]
    #[cfg_attr(doc_cfg, doc(cfg(target_has_atomic = "64")))]
    mod atomic_64 {
        use core::sync::atomic::{AtomicI64, AtomicU64};

        use super::*;

        impl_traits_for_atomics!(AtomicU64[u64], AtomicI64[i64]);

        const _: () = unsafe {
            unsafe_impl_transmute_from_for_atomic!(=> AtomicU64 [u64], => AtomicI64 [i64])
        };
    }

    #[cfg(target_has_atomic = "ptr")]
    #[cfg_attr(doc_cfg, doc(cfg(target_has_atomic = "ptr")))]
    mod atomic_ptr {
        use core::sync::atomic::{AtomicIsize, AtomicPtr, AtomicUsize};

        use super::*;

        impl_traits_for_atomics!(AtomicUsize[usize], AtomicIsize[isize]);

        impl_known_layout!(T => AtomicPtr<T>);

        impl_for_transmute_from!(T => TryFromBytes for AtomicPtr<T> [UnsafeCell<*mut T>]);
        impl_for_transmute_from!(T => FromZeros for AtomicPtr<T> [UnsafeCell<*mut T>]);

        const _: () = unsafe {
            unsafe_impl_transmute_from_for_atomic!(=> AtomicUsize [usize], => AtomicIsize [isize])
        };

        const _: () = unsafe { unsafe_impl_transmute_from_for_atomic!(T => AtomicPtr<T> [*mut T]) };
    }
}

const _: () = unsafe {
    unsafe_impl!(T: ?Sized => Immutable for PhantomData<T>);
    unsafe_impl!(T: ?Sized => TryFromBytes for PhantomData<T>);
    unsafe_impl!(T: ?Sized => FromZeros for PhantomData<T>);
    unsafe_impl!(T: ?Sized => FromBytes for PhantomData<T>);
    unsafe_impl!(T: ?Sized => IntoBytes for PhantomData<T>);
    unsafe_impl!(T: ?Sized => Unaligned for PhantomData<T>);
    assert_unaligned!(PhantomData<()>, PhantomData<u8>, PhantomData<u64>);
};

impl_for_transmute_from!(T: TryFromBytes => TryFromBytes for Wrapping<T>[<T>]);
impl_for_transmute_from!(T: FromZeros => FromZeros for Wrapping<T>[<T>]);
impl_for_transmute_from!(T: FromBytes => FromBytes for Wrapping<T>[<T>]);
impl_for_transmute_from!(T: IntoBytes => IntoBytes for Wrapping<T>[<T>]);
assert_unaligned!(Wrapping<()>, Wrapping<u8>);

const _: () = unsafe { unsafe_impl!(T: Immutable => Immutable for Wrapping<T>) };

const _: () = unsafe { unsafe_impl!(T: Unaligned => Unaligned for Wrapping<T>) };

const _: () = unsafe {
    unsafe_impl!(T => TryFromBytes for CoreMaybeUninit<T>);
    unsafe_impl!(T => FromZeros for CoreMaybeUninit<T>);
    unsafe_impl!(T => FromBytes for CoreMaybeUninit<T>);
};

const _: () = unsafe { unsafe_impl!(T: Immutable => Immutable for CoreMaybeUninit<T>) };

const _: () = unsafe { unsafe_impl!(T: Unaligned => Unaligned for CoreMaybeUninit<T>) };
assert_unaligned!(CoreMaybeUninit<()>, CoreMaybeUninit<u8>);

const _: () = unsafe { unsafe_impl!(T: ?Sized + Immutable => Immutable for ManuallyDrop<T>) };

impl_for_transmute_from!(T: ?Sized + TryFromBytes => TryFromBytes for ManuallyDrop<T>[<T>]);
impl_for_transmute_from!(T: ?Sized + FromZeros => FromZeros for ManuallyDrop<T>[<T>]);
impl_for_transmute_from!(T: ?Sized + FromBytes => FromBytes for ManuallyDrop<T>[<T>]);
impl_for_transmute_from!(T: ?Sized + IntoBytes => IntoBytes for ManuallyDrop<T>[<T>]);
const _: () = unsafe { unsafe_impl!(T: ?Sized + Unaligned => Unaligned for ManuallyDrop<T>) };
assert_unaligned!(ManuallyDrop<()>, ManuallyDrop<u8>);

impl_for_transmute_from!(T: ?Sized + TryFromBytes => TryFromBytes for Cell<T>[UnsafeCell<T>]);
impl_for_transmute_from!(T: ?Sized + FromZeros => FromZeros for Cell<T>[UnsafeCell<T>]);
impl_for_transmute_from!(T: ?Sized + FromBytes => FromBytes for Cell<T>[UnsafeCell<T>]);
impl_for_transmute_from!(T: ?Sized + IntoBytes => IntoBytes for Cell<T>[UnsafeCell<T>]);
const _: () = unsafe { unsafe_impl!(T: ?Sized + Unaligned => Unaligned for Cell<T>) };

impl_for_transmute_from!(T: ?Sized + FromZeros => FromZeros for UnsafeCell<T>[<T>]);
impl_for_transmute_from!(T: ?Sized + FromBytes => FromBytes for UnsafeCell<T>[<T>]);
impl_for_transmute_from!(T: ?Sized + IntoBytes => IntoBytes for UnsafeCell<T>[<T>]);
const _: () = unsafe { unsafe_impl!(T: ?Sized + Unaligned => Unaligned for UnsafeCell<T>) };
assert_unaligned!(UnsafeCell<()>, UnsafeCell<u8>);

unsafe impl<T: TryFromBytes + ?Sized> TryFromBytes for UnsafeCell<T> {
    #[allow(clippy::missing_inline_in_public_items)]
    fn only_derive_is_allowed_to_implement_this_trait()
    where
        Self: Sized,
    {
    }

    #[inline]
    fn is_bit_valid<A: invariant::Reference>(candidate: Maybe<'_, Self, A>) -> bool {
        let c = candidate.into_exclusive_or_pme();

        T::is_bit_valid(c.get_mut())
    }
}

const _: () = unsafe {
    unsafe_impl!(const N: usize, T: Immutable => Immutable for [T; N]);
    unsafe_impl!(const N: usize, T: TryFromBytes => TryFromBytes for [T; N]; |c| {
        <[T] as TryFromBytes>::is_bit_valid(c.as_slice())
    });
    unsafe_impl!(const N: usize, T: FromZeros => FromZeros for [T; N]);
    unsafe_impl!(const N: usize, T: FromBytes => FromBytes for [T; N]);
    unsafe_impl!(const N: usize, T: IntoBytes => IntoBytes for [T; N]);
    unsafe_impl!(const N: usize, T: Unaligned => Unaligned for [T; N]);
    assert_unaligned!([(); 0], [(); 1], [u8; 0], [u8; 1]);
    unsafe_impl!(T: Immutable => Immutable for [T]);
    unsafe_impl!(T: TryFromBytes => TryFromBytes for [T]; |c| {
        c.iter().all(<T as TryFromBytes>::is_bit_valid)
    });
    unsafe_impl!(T: FromZeros => FromZeros for [T]);
    unsafe_impl!(T: FromBytes => FromBytes for [T]);
    unsafe_impl!(T: IntoBytes => IntoBytes for [T]);
    unsafe_impl!(T: Unaligned => Unaligned for [T]);
};

const _: () = unsafe {
    unsafe_impl!(T: ?Sized => Immutable for *const T);
    unsafe_impl!(T: ?Sized => Immutable for *mut T);
    unsafe_impl!(T => TryFromBytes for *const T; |c| pointer::is_zeroed(c));
    unsafe_impl!(T => FromZeros for *const T);
    unsafe_impl!(T => TryFromBytes for *mut T; |c| pointer::is_zeroed(c));
    unsafe_impl!(T => FromZeros for *mut T);
};

const _: () = unsafe { unsafe_impl!(T: ?Sized => Immutable for NonNull<T>) };

const _: () = unsafe {
    unsafe_impl!(T: ?Sized => Immutable for &'_ T);
    unsafe_impl!(T: ?Sized => Immutable for &'_ mut T);
};

const _: () = unsafe { unsafe_impl!(T: Immutable => Immutable for Option<T>) };

#[cfg(feature = "simd")]
#[cfg_attr(doc_cfg, doc(cfg(feature = "simd")))]
mod simd {
    /// Defines a module which implements `TryFromBytes`, `FromZeros`,
    /// `FromBytes`, and `IntoBytes` for a set of types from a module in
    /// `core::arch`.
    ///
    /// `$arch` is both the name of the defined module and the name of the
    /// module in `core::arch`, and `$typ` is the list of items from that module
    /// to implement `FromZeros`, `FromBytes`, and `IntoBytes` for.
    #[allow(unused_macros)] 
    macro_rules! simd_arch_mod {
        ($(#[cfg $cfg:tt])* $(#[cfg_attr $cfg_attr:tt])? $arch:ident, $mod:ident, $($typ:ident),*) => {
            $(#[cfg $cfg])*
            #[cfg_attr(doc_cfg, doc(cfg $($cfg)*))]
            $(#[cfg_attr $cfg_attr])?
            mod $mod {
                use core::arch::$arch::{$($typ),*};

                use crate::*;
                impl_known_layout!($($typ),*);
                const _: () = unsafe {
                    $( unsafe_impl!($typ: Immutable, TryFromBytes, FromZeros, FromBytes, IntoBytes); )*
                };
            }
        };
    }

    #[rustfmt::skip]
    const _: () = {
        simd_arch_mod!(
            #[cfg(target_arch = "x86")]
            x86, x86, __m128, __m128d, __m128i, __m256, __m256d, __m256i
        );
        simd_arch_mod!(
            #[cfg(all(feature = "simd-nightly", target_arch = "x86"))]
            x86, x86_nightly, __m512bh, __m512, __m512d, __m512i
        );
        simd_arch_mod!(
            #[cfg(target_arch = "x86_64")]
            x86_64, x86_64, __m128, __m128d, __m128i, __m256, __m256d, __m256i
        );
        simd_arch_mod!(
            #[cfg(all(feature = "simd-nightly", target_arch = "x86_64"))]
            x86_64, x86_64_nightly, __m512bh, __m512, __m512d, __m512i
        );
        simd_arch_mod!(
#[cfg(any())]








            wasm32, wasm32, v128
        );
        simd_arch_mod!(
            #[cfg(all(feature = "simd-nightly", target_arch = "powerpc"))]
            powerpc, powerpc, vector_bool_long, vector_double, vector_signed_long, vector_unsigned_long
        );
        simd_arch_mod!(
            #[cfg(all(feature = "simd-nightly", target_arch = "powerpc64"))]
            powerpc64, powerpc64, vector_bool_long, vector_double, vector_signed_long, vector_unsigned_long
        );
        #[cfg(zerocopy_aarch64_simd_1_59_0)]
        simd_arch_mod!(
            #[cfg(all(target_arch = "aarch64", target_endian = "little"))]
            #[cfg_attr(doc_cfg, doc(cfg(rust = "1.59.0")))]
            aarch64, aarch64, float32x2_t, float32x4_t, float64x1_t, float64x2_t, int8x8_t, int8x8x2_t,
            int8x8x3_t, int8x8x4_t, int8x16_t, int8x16x2_t, int8x16x3_t, int8x16x4_t, int16x4_t,
            int16x8_t, int32x2_t, int32x4_t, int64x1_t, int64x2_t, poly8x8_t, poly8x8x2_t, poly8x8x3_t,
            poly8x8x4_t, poly8x16_t, poly8x16x2_t, poly8x16x3_t, poly8x16x4_t, poly16x4_t, poly16x8_t,
            poly64x1_t, poly64x2_t, uint8x8_t, uint8x8x2_t, uint8x8x3_t, uint8x8x4_t, uint8x16_t,
            uint8x16x2_t, uint8x16x3_t, uint8x16x4_t, uint16x4_t, uint16x8_t, uint32x2_t, uint32x4_t,
            uint64x1_t, uint64x2_t
        );
    };
}
