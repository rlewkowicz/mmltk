// Copyright 2024 The Fuchsia Authors
// Licensed under the 2-Clause BSD License <LICENSE-BSD or
// https://opensource.org/license/bsd-2-clause>, Apache License, Version 2.0
// <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
// license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.

/// Safely transmutes a value of one type to a value of another type of the same
/// size.
///
/// This macro behaves like an invocation of this function:
///
/// ```ignore
/// const fn transmute<Src, Dst>(src: Src) -> Dst
/// where
///     Src: IntoBytes,
///     Dst: FromBytes,
///     size_of::<Src>() == size_of::<Dst>(),
/// {
/// # /*
///     ...
/// # */
/// }
/// ```
///
/// However, unlike a function, this macro can only be invoked when the types of
/// `Src` and `Dst` are completely concrete. The types `Src` and `Dst` are
/// inferred from the calling context; they cannot be explicitly specified in
/// the macro invocation.
///
/// Note that the `Src` produced by the expression `$e` will *not* be dropped.
/// Semantically, its bits will be copied into a new value of type `Dst`, the
/// original `Src` will be forgotten, and the value of type `Dst` will be
/// returned.
///
/// # `#![allow(shrink)]`
///
/// If `#![allow(shrink)]` is provided, `transmute!` additionally supports
/// transmutations that shrink the size of the value; e.g.:
///
/// ```
/// # use zerocopy::transmute;
/// let u: u32 = transmute!(#![allow(shrink)] 0u64);
/// assert_eq!(u, 0u32);
/// ```
///
/// # Examples
///
/// ```
/// # use zerocopy::transmute;
/// let one_dimensional: [u8; 8] = [0, 1, 2, 3, 4, 5, 6, 7];
///
/// let two_dimensional: [[u8; 4]; 2] = transmute!(one_dimensional);
///
/// assert_eq!(two_dimensional, [[0, 1, 2, 3], [4, 5, 6, 7]]);
/// ```
///
/// # Use in `const` contexts
///
/// This macro can be invoked in `const` contexts.
#[macro_export]
macro_rules! transmute {
    (#![allow(shrink)] $e:expr) => {{
        let mut e = $e;
        if false {

            fn transmute<Src, Dst>(src: Src) -> Dst
            where
                Src: $crate::IntoBytes,
                Dst: $crate::FromBytes,
            {
                let _ = src;
                loop {}
            }
            loop {}
            #[allow(unreachable_code)]
            transmute(e)
        } else {
            use $crate::util::macro_util::core_reexport::mem::ManuallyDrop;

            #[repr(C, packed)]
            union Transmute<Src, Dst> {
                src: ManuallyDrop<Src>,
                dst: ManuallyDrop<Dst>,
            }

            let u: Transmute<_, _> = unsafe {
                #[allow(clippy::missing_transmute_annotations)]
                $crate::util::macro_util::core_reexport::mem::transmute(e)
            };

            if false {
                e = ManuallyDrop::into_inner(unsafe { u.src });
                let _ = e;
                loop {}
            } else {
                let dst = unsafe { u.dst };
                $crate::util::macro_util::must_use(ManuallyDrop::into_inner(dst))
            }
        }
    }};
    ($e:expr) => {{
        let e = $e;
        if false {

            fn transmute<Src, Dst>(src: Src) -> Dst
            where
                Src: $crate::IntoBytes,
                Dst: $crate::FromBytes,
            {
                let _ = src;
                loop {}
            }
            loop {}
            #[allow(unreachable_code)]
            transmute(e)
        } else {
            let u = unsafe {
                #[allow(clippy::missing_transmute_annotations, unnecessary_transmutes)]
                $crate::util::macro_util::core_reexport::mem::transmute(e)
            };
            $crate::util::macro_util::must_use(u)
        }
    }};
}

/// Safely transmutes a mutable or immutable reference of one type to an
/// immutable reference of another type of the same size and compatible
/// alignment.
///
/// This macro behaves like an invocation of this function:
///
/// ```ignore
/// fn transmute_ref<'src, 'dst, Src, Dst>(src: &'src Src) -> &'dst Dst
/// where
///     'src: 'dst,
///     Src: IntoBytes + Immutable + ?Sized,
///     Dst: FromBytes + Immutable + ?Sized,
///     align_of::<Src>() >= align_of::<Dst>(),
///     size_compatible::<Src, Dst>(),
/// {
/// # /*
///     ...
/// # */
/// }
/// ```
///
/// The types `Src` and `Dst` are inferred from the calling context; they cannot
/// be explicitly specified in the macro invocation.
///
/// # Size compatibility
///
/// `transmute_ref!` supports transmuting between `Sized` types or between
/// unsized (i.e., `?Sized`) types. It supports any transmutation that preserves
/// the number of bytes of the referent, even if doing so requires updating the
/// metadata stored in an unsized "fat" reference:
///
/// ```
/// # use zerocopy::transmute_ref;
/// # use core::mem::size_of_val; // Not in the prelude on our MSRV
/// let src: &[[u8; 2]] = &[[0, 1], [2, 3]][..];
/// let dst: &[u8] = transmute_ref!(src);
///
/// assert_eq!(src.len(), 2);
/// assert_eq!(dst.len(), 4);
/// assert_eq!(dst, [0, 1, 2, 3]);
/// assert_eq!(size_of_val(src), size_of_val(dst));
/// ```
///
/// # Errors
///
/// Violations of the alignment and size compatibility checks are detected
/// *after* the compiler performs monomorphization. This has two important
/// consequences.
///
/// First, it means that generic code will *never* fail these conditions:
///
/// ```
/// # use zerocopy::{transmute_ref, FromBytes, IntoBytes, Immutable};
/// fn transmute_ref<Src, Dst>(src: &Src) -> &Dst
/// where
///     Src: IntoBytes + Immutable,
///     Dst: FromBytes + Immutable,
/// {
///     transmute_ref!(src)
/// }
/// ```
///
/// Instead, failures will only be detected once generic code is instantiated
/// with concrete types:
///
/// ```compile_fail,E0080
/// # use zerocopy::{transmute_ref, FromBytes, IntoBytes, Immutable};
/// #
/// # fn transmute_ref<Src, Dst>(src: &Src) -> &Dst
/// # where
/// #     Src: IntoBytes + Immutable,
/// #     Dst: FromBytes + Immutable,
/// # {
/// #     transmute_ref!(src)
/// # }
/// let src: &u16 = &0;
/// let dst: &u8 = transmute_ref(src);
/// ```
///
/// Second, the fact that violations are detected after monomorphization means
/// that `cargo check` will usually not detect errors, even when types are
/// concrete. Instead, `cargo build` must be used to detect such errors.
///
/// # Examples
///
/// Transmuting between `Sized` types:
///
/// ```
/// # use zerocopy::transmute_ref;
/// let one_dimensional: [u8; 8] = [0, 1, 2, 3, 4, 5, 6, 7];
///
/// let two_dimensional: &[[u8; 4]; 2] = transmute_ref!(&one_dimensional);
///
/// assert_eq!(two_dimensional, &[[0, 1, 2, 3], [4, 5, 6, 7]]);
/// ```
///
/// Transmuting between unsized types:
///
/// ```
/// # use {zerocopy::*, zerocopy_derive::*};
/// # type u16 = zerocopy::byteorder::native_endian::U16;
/// # type u32 = zerocopy::byteorder::native_endian::U32;
/// #[derive(KnownLayout, FromBytes, IntoBytes, Immutable)]
/// #[repr(C)]
/// struct SliceDst<T, U> {
///     t: T,
///     u: [U],
/// }
///
/// type Src = SliceDst<u32, u16>;
/// type Dst = SliceDst<u16, u8>;
///
/// let src = Src::ref_from_bytes(&[0, 1, 2, 3, 4, 5, 6, 7]).unwrap();
/// let dst: &Dst = transmute_ref!(src);
///
/// assert_eq!(src.t.as_bytes(), [0, 1, 2, 3]);
/// assert_eq!(src.u.len(), 2);
/// assert_eq!(src.u.as_bytes(), [4, 5, 6, 7]);
///
/// assert_eq!(dst.t.as_bytes(), [0, 1]);
/// assert_eq!(dst.u, [2, 3, 4, 5, 6, 7]);
/// ```
///
/// # Use in `const` contexts
///
/// This macro can be invoked in `const` contexts only when `Src: Sized` and
/// `Dst: Sized`.
#[macro_export]
macro_rules! transmute_ref {
    ($e:expr) => {{

        let e: &_ = $e;

        #[allow(unused, clippy::diverging_sub_expression)]
        if false {

            struct AssertSrcIsIntoBytes<'a, T: ?::core::marker::Sized + $crate::IntoBytes>(&'a T);
            struct AssertSrcIsImmutable<'a, T: ?::core::marker::Sized + $crate::Immutable>(&'a T);
            struct AssertDstIsFromBytes<'a, U: ?::core::marker::Sized + $crate::FromBytes>(&'a U);
            struct AssertDstIsImmutable<'a, T: ?::core::marker::Sized + $crate::Immutable>(&'a T);

            let _ = AssertSrcIsIntoBytes(e);
            let _ = AssertSrcIsImmutable(e);

            if true {
                #[allow(unused, unreachable_code)]
                let u = AssertDstIsFromBytes(loop {});
                u.0
            } else {
                #[allow(unused, unreachable_code)]
                let u = AssertDstIsImmutable(loop {});
                u.0
            }
        } else {
            use $crate::util::macro_util::TransmuteRefDst;
            let t = $crate::util::macro_util::Wrap::new(e);
            unsafe {
                t.transmute_ref()
            }
        }
    }}
}

/// Safely transmutes a mutable reference of one type to a mutable reference of
/// another type of the same size and compatible alignment.
///
/// This macro behaves like an invocation of this function:
///
/// ```ignore
/// const fn transmute_mut<'src, 'dst, Src, Dst>(src: &'src mut Src) -> &'dst mut Dst
/// where
///     'src: 'dst,
///     Src: FromBytes + IntoBytes,
///     Dst: FromBytes + IntoBytes,
///     align_of::<Src>() >= align_of::<Dst>(),
///     size_compatible::<Src, Dst>(),
/// {
/// # /*
///     ...
/// # */
/// }
/// ```
///
/// The types `Src` and `Dst` are inferred from the calling context; they cannot
/// be explicitly specified in the macro invocation.
///
/// # Size compatibility
///
/// `transmute_mut!` supports transmuting between `Sized` types or between
/// unsized (i.e., `?Sized`) types. It supports any transmutation that preserves
/// the number of bytes of the referent, even if doing so requires updating the
/// metadata stored in an unsized "fat" reference:
///
/// ```
/// # use zerocopy::transmute_mut;
/// # use core::mem::size_of_val; // Not in the prelude on our MSRV
/// let src: &mut [[u8; 2]] = &mut [[0, 1], [2, 3]][..];
/// let dst: &mut [u8] = transmute_mut!(src);
///
/// assert_eq!(dst.len(), 4);
/// assert_eq!(dst, [0, 1, 2, 3]);
/// let dst_size = size_of_val(dst);
/// assert_eq!(src.len(), 2);
/// assert_eq!(size_of_val(src), dst_size);
/// ```
///
/// # Errors
///
/// Violations of the alignment and size compatibility checks are detected
/// *after* the compiler performs monomorphization. This has two important
/// consequences.
///
/// First, it means that generic code will *never* fail these conditions:
///
/// ```
/// # use zerocopy::{transmute_mut, FromBytes, IntoBytes, Immutable};
/// fn transmute_mut<Src, Dst>(src: &mut Src) -> &mut Dst
/// where
///     Src: FromBytes + IntoBytes,
///     Dst: FromBytes + IntoBytes,
/// {
///     transmute_mut!(src)
/// }
/// ```
///
/// Instead, failures will only be detected once generic code is instantiated
/// with concrete types:
///
/// ```compile_fail,E0080
/// # use zerocopy::{transmute_mut, FromBytes, IntoBytes, Immutable};
/// #
/// # fn transmute_mut<Src, Dst>(src: &mut Src) -> &mut Dst
/// # where
/// #     Src: FromBytes + IntoBytes,
/// #     Dst: FromBytes + IntoBytes,
/// # {
/// #     transmute_mut!(src)
/// # }
/// let src: &mut u16 = &mut 0;
/// let dst: &mut u8 = transmute_mut(src);
/// ```
///
/// Second, the fact that violations are detected after monomorphization means
/// that `cargo check` will usually not detect errors, even when types are
/// concrete. Instead, `cargo build` must be used to detect such errors.
///
///
/// # Examples
///
/// Transmuting between `Sized` types:
///
/// ```
/// # use zerocopy::transmute_mut;
/// let mut one_dimensional: [u8; 8] = [0, 1, 2, 3, 4, 5, 6, 7];
///
/// let two_dimensional: &mut [[u8; 4]; 2] = transmute_mut!(&mut one_dimensional);
///
/// assert_eq!(two_dimensional, &[[0, 1, 2, 3], [4, 5, 6, 7]]);
///
/// two_dimensional.reverse();
///
/// assert_eq!(one_dimensional, [4, 5, 6, 7, 0, 1, 2, 3]);
/// ```
///
/// Transmuting between unsized types:
///
/// ```
/// # use {zerocopy::*, zerocopy_derive::*};
/// # type u16 = zerocopy::byteorder::native_endian::U16;
/// # type u32 = zerocopy::byteorder::native_endian::U32;
/// #[derive(KnownLayout, FromBytes, IntoBytes, Immutable)]
/// #[repr(C)]
/// struct SliceDst<T, U> {
///     t: T,
///     u: [U],
/// }
///
/// type Src = SliceDst<u32, u16>;
/// type Dst = SliceDst<u16, u8>;
///
/// let mut bytes = [0, 1, 2, 3, 4, 5, 6, 7];
/// let src = Src::mut_from_bytes(&mut bytes[..]).unwrap();
/// let dst: &mut Dst = transmute_mut!(src);
///
/// assert_eq!(dst.t.as_bytes(), [0, 1]);
/// assert_eq!(dst.u, [2, 3, 4, 5, 6, 7]);
///
/// assert_eq!(src.t.as_bytes(), [0, 1, 2, 3]);
/// assert_eq!(src.u.len(), 2);
/// assert_eq!(src.u.as_bytes(), [4, 5, 6, 7]);
///
/// ```
#[macro_export]
macro_rules! transmute_mut {
    ($e:expr) => {{

        let e: &mut _ = $e;

        #[allow(unused)]
        use $crate::util::macro_util::TransmuteMutDst as _;
        let t = $crate::util::macro_util::Wrap::new(e);
        t.transmute_mut()
    }}
}

/// Conditionally transmutes a value of one type to a value of another type of
/// the same size.
///
/// This macro behaves like an invocation of this function:
///
/// ```ignore
/// fn try_transmute<Src, Dst>(src: Src) -> Result<Dst, ValidityError<Src, Dst>>
/// where
///     Src: IntoBytes,
///     Dst: TryFromBytes,
///     size_of::<Src>() == size_of::<Dst>(),
/// {
/// # /*
///     ...
/// # */
/// }
/// ```
///
/// However, unlike a function, this macro can only be invoked when the types of
/// `Src` and `Dst` are completely concrete. The types `Src` and `Dst` are
/// inferred from the calling context; they cannot be explicitly specified in
/// the macro invocation.
///
/// Note that the `Src` produced by the expression `$e` will *not* be dropped.
/// Semantically, its bits will be copied into a new value of type `Dst`, the
/// original `Src` will be forgotten, and the value of type `Dst` will be
/// returned.
///
/// # Examples
///
/// ```
/// # use zerocopy::*;
/// // 0u8 → bool = false
/// assert_eq!(try_transmute!(0u8), Ok(false));
///
/// // 1u8 → bool = true
///  assert_eq!(try_transmute!(1u8), Ok(true));
///
/// // 2u8 → bool = error
/// assert!(matches!(
///     try_transmute!(2u8),
///     Result::<bool, _>::Err(ValidityError { .. })
/// ));
/// ```
#[macro_export]
macro_rules! try_transmute {
    ($e:expr) => {{

        let e = $e;
        if false {

            Ok(unsafe {
                #[allow(clippy::missing_transmute_annotations)]
                $crate::util::macro_util::core_reexport::mem::transmute(e)
            })
        } else {
            $crate::util::macro_util::try_transmute::<_, _>(e)
        }
    }}
}

/// Conditionally transmutes a mutable or immutable reference of one type to an
/// immutable reference of another type of the same size and compatible
/// alignment.
///
/// This macro behaves like an invocation of this function:
///
/// ```ignore
/// fn try_transmute_ref<Src, Dst>(src: &Src) -> Result<&Dst, ValidityError<&Src, Dst>>
/// where
///     Src: IntoBytes + Immutable,
///     Dst: TryFromBytes + Immutable,
///     size_of::<Src>() == size_of::<Dst>(),
///     align_of::<Src>() >= align_of::<Dst>(),
/// {
/// # /*
///     ...
/// # */
/// }
/// ```
///
/// However, unlike a function, this macro can only be invoked when the types of
/// `Src` and `Dst` are completely concrete. The types `Src` and `Dst` are
/// inferred from the calling context; they cannot be explicitly specified in
/// the macro invocation.
///
/// # Examples
///
/// ```
/// # use zerocopy::*;
/// // 0u8 → bool = false
/// assert_eq!(try_transmute_ref!(&0u8), Ok(&false));
///
/// // 1u8 → bool = true
///  assert_eq!(try_transmute_ref!(&1u8), Ok(&true));
///
/// // 2u8 → bool = error
/// assert!(matches!(
///     try_transmute_ref!(&2u8),
///     Result::<&bool, _>::Err(ValidityError { .. })
/// ));
/// ```
///
/// # Alignment increase error message
///
/// Because of limitations on macros, the error message generated when
/// `try_transmute_ref!` is used to transmute from a type of lower alignment to
/// a type of higher alignment is somewhat confusing. For example, the following
/// code:
///
/// ```compile_fail
/// let increase_alignment: Result<&u16, _> = zerocopy::try_transmute_ref!(&[0u8; 2]);
/// ```
///
/// ...generates the following error:
///
/// ```text
/// error[E0512]: cannot transmute between types of different sizes, or dependently-sized types
///  --> example.rs:1:47
///   |
/// 1 |     let increase_alignment: Result<&u16, _> = zerocopy::try_transmute_ref!(&[0u8; 2]);
///   |                                               ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
///   |
///   = note: source type: `AlignOf<[u8; 2]>` (8 bits)
///   = note: target type: `MaxAlignsOf<[u8; 2], u16>` (16 bits)
///   = note: this error originates in the macro `$crate::assert_align_gt_eq` which comes from the expansion of the macro `zerocopy::try_transmute_ref` (in Nightly builds, run with -Z macro-backtrace for more info)/// ```
/// ```
///
/// This is saying that `max(align_of::<T>(), align_of::<U>()) !=
/// align_of::<T>()`, which is equivalent to `align_of::<T>() <
/// align_of::<U>()`.
#[macro_export]
macro_rules! try_transmute_ref {
    ($e:expr) => {{

        let e: &_ = $e;

        #[allow(unreachable_code, unused, clippy::diverging_sub_expression)]
        if false {

            let mut t = loop {};
            e = &t;

            let u;

            $crate::assert_size_eq!(t, u);
            $crate::assert_align_gt_eq!(t, u);

            Ok(&u)
        } else {
            $crate::util::macro_util::try_transmute_ref::<_, _>(e)
        }
    }}
}

/// Conditionally transmutes a mutable reference of one type to a mutable
/// reference of another type of the same size and compatible alignment.
///
/// This macro behaves like an invocation of this function:
///
/// ```ignore
/// fn try_transmute_mut<Src, Dst>(src: &mut Src) -> Result<&mut Dst, ValidityError<&mut Src, Dst>>
/// where
///     Src: FromBytes + IntoBytes,
///     Dst: TryFromBytes + IntoBytes,
///     size_of::<Src>() == size_of::<Dst>(),
///     align_of::<Src>() >= align_of::<Dst>(),
/// {
/// # /*
///     ...
/// # */
/// }
/// ```
///
/// However, unlike a function, this macro can only be invoked when the types of
/// `Src` and `Dst` are completely concrete. The types `Src` and `Dst` are
/// inferred from the calling context; they cannot be explicitly specified in
/// the macro invocation.
///
/// # Examples
///
/// ```
/// # use zerocopy::*;
/// // 0u8 → bool = false
/// let src = &mut 0u8;
/// assert_eq!(try_transmute_mut!(src), Ok(&mut false));
///
/// // 1u8 → bool = true
/// let src = &mut 1u8;
///  assert_eq!(try_transmute_mut!(src), Ok(&mut true));
///
/// // 2u8 → bool = error
/// let src = &mut 2u8;
/// assert!(matches!(
///     try_transmute_mut!(src),
///     Result::<&mut bool, _>::Err(ValidityError { .. })
/// ));
/// ```
///
/// # Alignment increase error message
///
/// Because of limitations on macros, the error message generated when
/// `try_transmute_ref!` is used to transmute from a type of lower alignment to
/// a type of higher alignment is somewhat confusing. For example, the following
/// code:
///
/// ```compile_fail
/// let src = &mut [0u8; 2];
/// let increase_alignment: Result<&mut u16, _> = zerocopy::try_transmute_mut!(src);
/// ```
///
/// ...generates the following error:
///
/// ```text
/// error[E0512]: cannot transmute between types of different sizes, or dependently-sized types
///  --> example.rs:2:51
///   |
/// 2 |     let increase_alignment: Result<&mut u16, _> = zerocopy::try_transmute_mut!(src);
///   |                                                   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
///   |
///   = note: source type: `AlignOf<[u8; 2]>` (8 bits)
///   = note: target type: `MaxAlignsOf<[u8; 2], u16>` (16 bits)
///   = note: this error originates in the macro `$crate::assert_align_gt_eq` which comes from the expansion of the macro `zerocopy::try_transmute_mut` (in Nightly builds, run with -Z macro-backtrace for more info)
/// ```
///
/// This is saying that `max(align_of::<T>(), align_of::<U>()) !=
/// align_of::<T>()`, which is equivalent to `align_of::<T>() <
/// align_of::<U>()`.
#[macro_export]
macro_rules! try_transmute_mut {
    ($e:expr) => {{

        let e: &mut _ = $e;

        #[allow(unreachable_code, unused, clippy::diverging_sub_expression)]
        if false {

            let mut t = loop {};
            e = &mut t;

            let u;

            $crate::assert_size_eq!(t, u);
            $crate::assert_align_gt_eq!(t, u);

            Ok(&mut u)
        } else {
            $crate::util::macro_util::try_transmute_mut::<_, _>(e)
        }
    }}
}

/// Includes a file and safely transmutes it to a value of an arbitrary type.
///
/// The file will be included as a byte array, `[u8; N]`, which will be
/// transmuted to another type, `T`. `T` is inferred from the calling context,
/// and must implement [`FromBytes`].
///
/// The file is located relative to the current file (similarly to how modules
/// are found). The provided path is interpreted in a platform-specific way at
/// compile time. So, for instance, an invocation with a Windows path containing
/// backslashes `\` would not compile correctly on Unix.
///
/// `include_value!` is ignorant of byte order. For byte order-aware types, see
/// the [`byteorder`] module.
///
/// [`FromBytes`]: crate::FromBytes
/// [`byteorder`]: crate::byteorder
///
/// # Examples
///
/// Assume there are two files in the same directory with the following
/// contents:
///
/// File `data` (no trailing newline):
///
/// ```text
/// abcd
/// ```
///
/// File `main.rs`:
///
/// ```rust
/// use zerocopy::include_value;
/// # macro_rules! include_value {
/// # ($file:expr) => { zerocopy::include_value!(concat!("../testdata/include_value/", $file)) };
/// # }
///
/// fn main() {
///     let as_u32: u32 = include_value!("data");
///     assert_eq!(as_u32, u32::from_ne_bytes([b'a', b'b', b'c', b'd']));
///     let as_i32: i32 = include_value!("data");
///     assert_eq!(as_i32, i32::from_ne_bytes([b'a', b'b', b'c', b'd']));
/// }
/// ```
///
/// # Use in `const` contexts
///
/// This macro can be invoked in `const` contexts.
#[doc(alias("include_bytes", "include_data", "include_type"))]
#[macro_export]
macro_rules! include_value {
    ($file:expr $(,)?) => {
        $crate::transmute!(*::core::include_bytes!($file))
    };
}

#[doc(hidden)]
#[macro_export]
macro_rules! cryptocorrosion_derive_traits {
    (
        #[repr($repr:ident)]
        $(#[$attr:meta])*
        $vis:vis struct $name:ident $(<$($tyvar:ident),*>)?
        $(
            (
                $($tuple_field_vis:vis $tuple_field_ty:ty),*
            );
        )?

        $(
            {
                $($field_vis:vis $field_name:ident: $field_ty:ty,)*
            }
        )?
    ) => {
        $crate::cryptocorrosion_derive_traits!(@assert_allowed_struct_repr #[repr($repr)]);

        $(#[$attr])*
        #[repr($repr)]
        $vis struct $name $(<$($tyvar),*>)?
        $(
            (
                $($tuple_field_vis $tuple_field_ty),*
            );
        )?

        $(
            {
                $($field_vis $field_name: $field_ty,)*
            }
        )?

        unsafe impl $(<$($tyvar),*>)? $crate::TryFromBytes for $name$(<$($tyvar),*>)?
        where
            $(
                $($tuple_field_ty: $crate::FromBytes,)*
            )?

            $(
                $($field_ty: $crate::FromBytes,)*
            )?
        {
            fn is_bit_valid<A>(_c: $crate::Maybe<'_, Self, A>) -> bool
            where
                A: $crate::pointer::invariant::Reference
            {
                true
            }

            fn only_derive_is_allowed_to_implement_this_trait() {}
        }

        unsafe impl $(<$($tyvar),*>)? $crate::FromZeros for $name$(<$($tyvar),*>)?
        where
            $(
                $($tuple_field_ty: $crate::FromBytes,)*
            )?

            $(
                $($field_ty: $crate::FromBytes,)*
            )?
        {
            fn only_derive_is_allowed_to_implement_this_trait() {}
        }

        unsafe impl $(<$($tyvar),*>)? $crate::FromBytes for $name$(<$($tyvar),*>)?
        where
            $(
                $($tuple_field_ty: $crate::FromBytes,)*
            )?

            $(
                $($field_ty: $crate::FromBytes,)*
            )?
        {
            fn only_derive_is_allowed_to_implement_this_trait() {}
        }

        unsafe impl $(<$($tyvar),*>)? $crate::IntoBytes for $name$(<$($tyvar),*>)?
        where
            $(
                $($tuple_field_ty: $crate::IntoBytes,)*
            )?

            $(
                $($field_ty: $crate::IntoBytes,)*
            )?

            (): $crate::util::macro_util::PaddingFree<
                Self,
                {
                    $crate::cryptocorrosion_derive_traits!(
                        @struct_padding_check #[repr($repr)]
                        $(($($tuple_field_ty),*))?
                        $({$($field_ty),*})?
                    )
                },
            >,
        {
            fn only_derive_is_allowed_to_implement_this_trait() {}
        }

        unsafe impl $(<$($tyvar),*>)? $crate::Immutable for $name$(<$($tyvar),*>)?
        where
            $(
                $($tuple_field_ty: $crate::Immutable,)*
            )?

            $(
                $($field_ty: $crate::Immutable,)*
            )?
        {
            fn only_derive_is_allowed_to_implement_this_trait() {}
        }
    };
    (@assert_allowed_struct_repr #[repr(transparent)]) => {};
    (@assert_allowed_struct_repr #[repr(C)]) => {};
    (@assert_allowed_struct_repr #[$_attr:meta]) => {
        compile_error!("repr must be `#[repr(transparent)]` or `#[repr(C)]`");
    };
    (
        @struct_padding_check #[repr(transparent)]
        $(($($tuple_field_ty:ty),*))?
        $({$($field_ty:ty),*})?
    ) => {
        0
    };
    (
        @struct_padding_check #[repr(C)]
        $(($($tuple_field_ty:ty),*))?
        $({$($field_ty:ty),*})?
    ) => {
        $crate::struct_padding!(
            Self,
            [
                $($($tuple_field_ty),*)?
                $($($field_ty),*)?
            ]
        )
    };
    (
        #[repr(C)]
        $(#[$attr:meta])*
        $vis:vis union $name:ident {
            $(
                $field_name:ident: $field_ty:ty,
            )*
        }
    ) => {
        $(#[$attr])*
        #[repr(C)]
        $vis union $name {
            $(
                $field_name: $field_ty,
            )*
        }

        unsafe impl $crate::TryFromBytes for $name
        where
            $(
                $field_ty: $crate::FromBytes,
            )*
        {
            fn is_bit_valid<A>(_c: $crate::Maybe<'_, Self, A>) -> bool
            where
                A: $crate::pointer::invariant::Reference
            {
                true
            }

            fn only_derive_is_allowed_to_implement_this_trait() {}
        }

        unsafe impl $crate::FromZeros for $name
        where
            $(
                $field_ty: $crate::FromBytes,
            )*
        {
            fn only_derive_is_allowed_to_implement_this_trait() {}
        }

        unsafe impl $crate::FromBytes for $name
        where
            $(
                $field_ty: $crate::FromBytes,
            )*
        {
            fn only_derive_is_allowed_to_implement_this_trait() {}
        }

        unsafe impl $crate::IntoBytes for $name
        where
            $(
                $field_ty: $crate::IntoBytes,
            )*
            (): $crate::util::macro_util::PaddingFree<
                Self,
                {
                    $crate::union_padding!(
                        Self,
                        [$($field_ty),*]
                    )
                },
            >,
        {
            fn only_derive_is_allowed_to_implement_this_trait() {}
        }

        unsafe impl $crate::Immutable for $name
        where
            $(
                $field_ty: $crate::Immutable,
            )*
        {
            fn only_derive_is_allowed_to_implement_this_trait() {}
        }
    };
}
