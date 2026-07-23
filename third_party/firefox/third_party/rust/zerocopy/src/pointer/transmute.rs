// Copyright 2025 The Fuchsia Authors
// Licensed under a BSD-style license <LICENSE-BSD>, Apache License, Version 2.0
// <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
// license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.

use core::{
    cell::{Cell, UnsafeCell},
    mem::{ManuallyDrop, MaybeUninit},
    num::Wrapping,
};

use crate::{
    pointer::{invariant::*, PtrInner},
    FromBytes, Immutable, IntoBytes, Unalign,
};

/// Transmutations which are sound to attempt, conditional on validating the bit
/// validity of the destination type.
///
/// If a `Ptr` transmutation is `TryTransmuteFromPtr`, then it is sound to
/// perform that transmutation so long as some additional mechanism is used to
/// validate that the referent is bit-valid for the destination type. That
/// validation mechanism could be a type bound (such as `TransmuteFrom`) or a
/// runtime validity check.
///
/// # Safety
///
/// ## Post-conditions
///
/// Given `Dst: TryTransmuteFromPtr<Src, A, SV, DV, _>`, callers may assume the
/// following:
///
/// Given `src: Ptr<'a, Src, (A, _, SV)>`, if the referent of `src` is
/// `DV`-valid for `Dst`, then it is sound to transmute `src` into `dst: Ptr<'a,
/// Dst, (A, Unaligned, DV)>` by preserving pointer address and metadata.
///
/// ## Pre-conditions
///
/// Given `src: Ptr<Src, (A, _, SV)>` and `dst: Ptr<Dst, (A, Unaligned, DV)>`,
/// `Dst: TryTransmuteFromPtr<Src, A, SV, DV, _>` is sound if all of the
/// following hold:
/// - Forwards transmutation: Either of the following hold:
///   - So long as `dst` is active, no mutation of `dst`'s referent is allowed
///     except via `dst` itself
///   - The set of `DV`-valid `Dst`s is a superset of the set of `SV`-valid
///     `Src`s
/// - Reverse transmutation: Either of the following hold:
///   - `dst` does not permit mutation of its referent
///   - The set of `DV`-valid `Dst`s is a subset of the set of `SV`-valid `Src`s
/// - No safe code, given access to `src` and `dst`, can cause undefined
///   behavior: Any of the following hold:
///   - `A` is `Exclusive`
///   - `Src: Immutable` and `Dst: Immutable`
///   - It is sound for shared code to operate on a `&Src` and `&Dst` which
///     reference the same byte range at the same time
///
/// ## Proof
///
/// Given:
/// - `src: Ptr<'a, Src, (A, _, SV)>`
/// - `src`'s referent is `DV`-valid for `Dst`
/// - `Dst: SizeEq<Src>`
///
/// We are trying to prove that it is sound to perform a pointer address- and
/// metadata-preserving transmute from `src` to a `dst: Ptr<'a, Dst, (A,
/// Unaligned, DV)>`. We need to prove that such a transmute does not violate
/// any of `src`'s invariants, and that it satisfies all invariants of the
/// destination `Ptr` type.
///
/// First, all of `src`'s `PtrInner` invariants are upheld. `src`'s address and
/// metadata are unchanged, so:
/// - If its referent is not zero sized, then it still has valid provenance for
///   its referent, which is still entirely contained in some Rust allocation,
///   `A`
/// - If its referent is not zero sized, `A` is guaranteed to live for at least
///   `'a`
///
/// Since `Dst: SizeEq<Src>`, and since `dst` has the same address and metadata
/// as `src`, `dst` addresses the same byte range as `src`. `dst` also has the
/// same lifetime as `src`. Therefore, all of the `PtrInner` invariants
/// mentioned above also hold for `dst`.
///
/// Second, since `src`'s address is unchanged, it still satisfies its
/// alignment. Since `dst`'s alignment is `Unaligned`, it trivially satisfies
/// its alignment.
///
/// Third, aliasing is either `Exclusive` or `Shared`:
/// - If it is `Exclusive`, then both `src` and `dst` satisfy `Exclusive`
///   aliasing trivially: since `src` and `dst` have the same lifetime, `src` is
///   inaccessible so long as `dst` is alive, and no other live `Ptr`s or
///   references may reference the same referent.
/// - If it is `Shared`, then either:
///   - `Src: Immutable` and `Dst: Immutable`, and so `UnsafeCell`s trivially
///     cover the same byte ranges in both types.
///   - It is explicitly sound for safe code to operate on a `&Src` and a `&Dst`
///     pointing to the same byte range at the same time.
///
/// Fourth, `src`'s validity is satisfied. By invariant, `src`'s referent began
/// as an `SV`-valid `Src`. It is guaranteed to remain so, as either of the
/// following hold:
/// - `dst` does not permit mutation of its referent.
/// - The set of `DV`-valid `Dst`s is a superset of the set of `SV`-valid
///   `Src`s. Thus, any value written via `dst` is guaranteed to be `SV`-valid
///   for `Src`.
///
/// Fifth, `dst`'s validity is satisfied. It is a given of this proof that the
/// referent is `DV`-valid for `Dst`. It is guaranteed to remain so, as either
/// of the following hold:
/// - So long as `dst` is active, no mutation of the referent is allowed except
///   via `dst` itself.
/// - The set of `DV`-valid `Dst`s is a superset of the set of `SV`-valid
///   `Src`s. Thus, any value written via `src` is guaranteed to be a `DV`-valid
///   `Dst`.
pub unsafe trait TryTransmuteFromPtr<Src: ?Sized, A: Aliasing, SV: Validity, DV: Validity, R>:
    SizeEq<Src>
{
}

#[allow(missing_copy_implementations, missing_debug_implementations)]
pub enum BecauseMutationCompatible {}

unsafe impl<Src, Dst, SV, DV, A, R>
    TryTransmuteFromPtr<Src, A, SV, DV, (BecauseMutationCompatible, R)> for Dst
where
    A: Aliasing,
    SV: Validity,
    DV: Validity,
    Src: TransmuteFrom<Dst, DV, SV> + ?Sized,
    Dst: MutationCompatible<Src, A, SV, DV, R> + SizeEq<Src> + ?Sized,
{
}

unsafe impl<Src, Dst, SV, DV> TryTransmuteFromPtr<Src, Shared, SV, DV, BecauseImmutable> for Dst
where
    SV: Validity,
    DV: Validity,
    Src: Immutable + ?Sized,
    Dst: Immutable + SizeEq<Src> + ?Sized,
{
}

/// Denotes that `src: Ptr<Src, (A, _, SV)>` and `dst: Ptr<Self, (A, _, DV)>`,
/// referencing the same referent at the same time, cannot be used by safe code
/// to break library safety invariants of `Src` or `Self`.
///
/// # Safety
///
/// At least one of the following must hold:
/// - `Src: Read<A, _>` and `Self: Read<A, _>`
/// - `Self: InvariantsEq<Src>`, and, for some `V`:
///   - `Dst: TransmuteFrom<Src, V, V>`
///   - `Src: TransmuteFrom<Dst, V, V>`
pub unsafe trait MutationCompatible<Src: ?Sized, A: Aliasing, SV, DV, R> {}

#[allow(missing_copy_implementations, missing_debug_implementations)]
pub enum BecauseRead {}

unsafe impl<Src: ?Sized, Dst: ?Sized, A: Aliasing, SV: Validity, DV: Validity, R, S>
    MutationCompatible<Src, A, SV, DV, (BecauseRead, (R, S))> for Dst
where
    Src: Read<A, R>,
    Dst: Read<A, S>,
{
}

/// Denotes that two types have the same invariants.
///
/// # Safety
///
/// It is sound for safe code to operate on a `&T` and a `&Self` pointing to the
/// same referent at the same time - no such safe code can cause undefined
/// behavior.
pub unsafe trait InvariantsEq<T: ?Sized> {}

unsafe impl<T: ?Sized> InvariantsEq<T> for T {}

unsafe impl<Src: ?Sized, Dst: ?Sized, A: Aliasing, SV: Validity, DV: Validity>
    MutationCompatible<Src, A, SV, DV, BecauseInvariantsEq> for Dst
where
    Src: TransmuteFrom<Dst, DV, SV>,
    Dst: TransmuteFrom<Src, SV, DV> + InvariantsEq<Src>,
{
}

pub(crate) enum BecauseInvariantsEq {}

macro_rules! unsafe_impl_invariants_eq {
    ($tyvar:ident => $t:ty, $u:ty) => {{
        crate::util::macros::__unsafe();
        unsafe impl<$tyvar> InvariantsEq<$t> for $u {}
        unsafe impl<$tyvar> InvariantsEq<$u> for $t {}
    }};
}

impl_transitive_transmute_from!(T => MaybeUninit<T> => T => Wrapping<T>);
impl_transitive_transmute_from!(T => Wrapping<T> => T => MaybeUninit<T>);

unsafe impl<T: ?Sized> InvariantsEq<T> for ManuallyDrop<T> {}
unsafe impl<T: ?Sized> InvariantsEq<ManuallyDrop<T>> for T {}

/// Transmutations which are always sound.
///
/// `TransmuteFromPtr` is a shorthand for [`TryTransmuteFromPtr`] and
/// [`TransmuteFrom`].
///
/// # Safety
///
/// `Dst: TransmuteFromPtr<Src, A, SV, DV, _>` is equivalent to `Dst:
/// TryTransmuteFromPtr<Src, A, SV, DV, _> + TransmuteFrom<Src, SV, DV>`.
pub unsafe trait TransmuteFromPtr<Src: ?Sized, A: Aliasing, SV: Validity, DV: Validity, R>:
    TryTransmuteFromPtr<Src, A, SV, DV, R> + TransmuteFrom<Src, SV, DV>
{
}

unsafe impl<Src: ?Sized, Dst: ?Sized, A: Aliasing, SV: Validity, DV: Validity, R>
    TransmuteFromPtr<Src, A, SV, DV, R> for Dst
where
    Dst: TransmuteFrom<Src, SV, DV> + TryTransmuteFromPtr<Src, A, SV, DV, R>,
{
}

/// Denotes that any `SV`-valid `Src` may soundly be transmuted into a
/// `DV`-valid `Self`.
///
/// # Safety
///
/// Given `src: Ptr<Src, (_, _, SV)>` and `dst: Ptr<Dst, (_, _, DV)>`, if the
/// referents of `src` and `dst` are the same size, then the set of bit patterns
/// allowed to appear in `src`'s referent must be a subset of the set allowed to
/// appear in `dst`'s referent.
///
/// If the referents are not the same size, then `Dst: TransmuteFrom<Src, SV,
/// DV>` conveys no safety guarantee.
pub unsafe trait TransmuteFrom<Src: ?Sized, SV, DV> {}

/// # Safety
///
/// `T` and `Self` must have the same vtable kind (`Sized`, slice DST, `dyn`,
/// etc) and have the same size. In particular:
/// - If `T: Sized` and `Self: Sized`, then their sizes must be equal
/// - If `T: ?Sized` and `Self: ?Sized`, then it must be the case that, given
///   any `t: PtrInner<'_, T>`, `<Self as SizeEq<T>>::cast_from_raw(t)` produces
///   a pointer which addresses the same number of bytes as `t`. *Note that it
///   is **not** guaranteed that an `as` cast preserves referent size: it may be
///   the case that `cast_from_raw` modifies the pointer's metadata in order to
///   preserve referent size, which an `as` cast does not do.*
pub unsafe trait SizeEq<T: ?Sized> {
    fn cast_from_raw(t: PtrInner<'_, T>) -> PtrInner<'_, Self>;
}

unsafe impl<T: ?Sized> SizeEq<T> for T {
    #[inline(always)]
    fn cast_from_raw(t: PtrInner<'_, T>) -> PtrInner<'_, T> {
        t
    }
}

unsafe impl<Src, Dst> TransmuteFrom<Src, Valid, Initialized> for Dst
where
    Src: IntoBytes + ?Sized,
    Dst: ?Sized,
{
}

unsafe impl<Src, Dst> TransmuteFrom<Src, Initialized, Valid> for Dst
where
    Src: ?Sized,
    Dst: FromBytes + ?Sized,
{
}


unsafe impl<Src, Dst> TransmuteFrom<Src, Initialized, Initialized> for Dst
where
    Src: ?Sized,
    Dst: ?Sized,
{
}


unsafe impl<Src, Dst, V> TransmuteFrom<Src, V, Uninit> for Dst
where
    Src: ?Sized,
    Dst: ?Sized,
    V: Validity,
{
}

const _: () = unsafe { unsafe_impl_for_transparent_wrapper!(T: ?Sized => ManuallyDrop<T>) };

const _: () = unsafe { unsafe_impl_for_transparent_wrapper!(T => Unalign<T>) };
const _: () = unsafe { unsafe_impl_invariants_eq!(T => T, Unalign<T>) };

const _: () = unsafe { unsafe_impl_for_transparent_wrapper!(T => Wrapping<T>) };

const _: () = unsafe { unsafe_impl_invariants_eq!(T => T, Wrapping<T>) };

const _: () = unsafe { unsafe_impl_for_transparent_wrapper!(T: ?Sized => UnsafeCell<T>) };

const _: () = unsafe { unsafe_impl_for_transparent_wrapper!(T: ?Sized => Cell<T>) };

impl_transitive_transmute_from!(T: ?Sized => Cell<T> => T => UnsafeCell<T>);
impl_transitive_transmute_from!(T: ?Sized => UnsafeCell<T> => T => Cell<T>);

unsafe impl<T> TransmuteFrom<T, Uninit, Valid> for MaybeUninit<T> {}

unsafe impl<T> SizeEq<T> for MaybeUninit<T> {
    #[inline(always)]
    fn cast_from_raw(t: PtrInner<'_, T>) -> PtrInner<'_, MaybeUninit<T>> {
        unsafe { cast!(t) }
    }
}

unsafe impl<T> SizeEq<MaybeUninit<T>> for T {
    #[inline(always)]
    fn cast_from_raw(t: PtrInner<'_, MaybeUninit<T>>) -> PtrInner<'_, T> {
        unsafe { cast!(t) }
    }
}
