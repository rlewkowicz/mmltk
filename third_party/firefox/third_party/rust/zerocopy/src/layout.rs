// Copyright 2024 The Fuchsia Authors
// Licensed under the 2-Clause BSD License <LICENSE-BSD or
// https://opensource.org/license/bsd-2-clause>, Apache License, Version 2.0
// <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
// license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.

use core::{mem, num::NonZeroUsize};

use crate::util;

/// The target pointer width, counted in bits.
const POINTER_WIDTH_BITS: usize = mem::size_of::<usize>() * 8;

/// The layout of a type which might be dynamically-sized.
///
/// `DstLayout` describes the layout of sized types, slice types, and "slice
/// DSTs" - ie, those that are known by the type system to have a trailing slice
/// (as distinguished from `dyn Trait` types - such types *might* have a
/// trailing slice type, but the type system isn't aware of it).
///
/// Note that `DstLayout` does not have any internal invariants, so no guarantee
/// is made that a `DstLayout` conforms to any of Rust's requirements regarding
/// the layout of real Rust types or instances of types.
#[doc(hidden)]
#[allow(missing_debug_implementations, missing_copy_implementations)]
#[cfg_attr(kani, derive(Debug, PartialEq, Eq))]
#[derive(Copy, Clone)]
pub struct DstLayout {
    pub(crate) align: NonZeroUsize,
    pub(crate) size_info: SizeInfo,
    pub(crate) statically_shallow_unpadded: bool,
}

#[cfg_attr(kani, derive(Debug, PartialEq, Eq))]
#[derive(Copy, Clone)]
pub(crate) enum SizeInfo<E = usize> {
    Sized { size: usize },
    SliceDst(TrailingSliceLayout<E>),
}

#[cfg_attr(kani, derive(Debug, PartialEq, Eq))]
#[derive(Copy, Clone)]
pub(crate) struct TrailingSliceLayout<E = usize> {
    pub(crate) offset: usize,
    pub(crate) elem_size: E,
}

impl SizeInfo {
    /// Attempts to create a `SizeInfo` from `Self` in which `elem_size` is a
    /// `NonZeroUsize`. If `elem_size` is 0, returns `None`.
    #[allow(unused)]
    const fn try_to_nonzero_elem_size(&self) -> Option<SizeInfo<NonZeroUsize>> {
        Some(match *self {
            SizeInfo::Sized { size } => SizeInfo::Sized { size },
            SizeInfo::SliceDst(TrailingSliceLayout { offset, elem_size }) => {
                if let Some(elem_size) = NonZeroUsize::new(elem_size) {
                    SizeInfo::SliceDst(TrailingSliceLayout { offset, elem_size })
                } else {
                    return None;
                }
            }
        })
    }
}

#[doc(hidden)]
#[derive(Copy, Clone)]
#[allow(missing_debug_implementations)]
pub enum CastType {
    Prefix,
    Suffix,
}

pub(crate) enum MetadataCastError {
    Alignment,
    Size,
}

impl DstLayout {
    /// The minimum possible alignment of a type.
    const MIN_ALIGN: NonZeroUsize = match NonZeroUsize::new(1) {
        Some(min_align) => min_align,
        None => const_unreachable!(),
    };

    /// The maximum theoretic possible alignment of a type.
    ///
    /// For compatibility with future Rust versions, this is defined as the
    /// maximum power-of-two that fits into a `usize`. See also
    /// [`DstLayout::CURRENT_MAX_ALIGN`].
    pub(crate) const THEORETICAL_MAX_ALIGN: NonZeroUsize =
        match NonZeroUsize::new(1 << (POINTER_WIDTH_BITS - 1)) {
            Some(max_align) => max_align,
            None => const_unreachable!(),
        };

    /// The current, documented max alignment of a type \[1\].
    ///
    /// \[1\] Per <https://doc.rust-lang.org/reference/type-layout.html#the-alignment-modifiers>:
    ///
    ///   The alignment value must be a power of two from 1 up to
    ///   2<sup>29</sup>.
    #[cfg(not(kani))]
    #[cfg(not(target_pointer_width = "16"))]
    pub(crate) const CURRENT_MAX_ALIGN: NonZeroUsize = match NonZeroUsize::new(1 << 28) {
        Some(max_align) => max_align,
        None => const_unreachable!(),
    };

    #[cfg(not(kani))]
    #[cfg(target_pointer_width = "16")]
    pub(crate) const CURRENT_MAX_ALIGN: NonZeroUsize = match NonZeroUsize::new(1 << 15) {
        Some(max_align) => max_align,
        None => const_unreachable!(),
    };

    /// Assumes that this layout lacks static shallow padding.
    ///
    /// # Panics
    ///
    /// This method does not panic.
    ///
    /// # Safety
    ///
    /// If `self` describes the size and alignment of type that lacks static
    /// shallow padding, unsafe code may assume that the result of this method
    /// accurately reflects the size, alignment, and lack of static shallow
    /// padding of that type.
    const fn assume_shallow_unpadded(self) -> Self {
        Self { statically_shallow_unpadded: true, ..self }
    }

    /// Constructs a `DstLayout` for a zero-sized type with `repr_align`
    /// alignment (or 1). If `repr_align` is provided, then it must be a power
    /// of two.
    ///
    /// # Panics
    ///
    /// This function panics if the supplied `repr_align` is not a power of two.
    ///
    /// # Safety
    ///
    /// Unsafe code may assume that the contract of this function is satisfied.
    #[doc(hidden)]
    #[must_use]
    #[inline]
    pub const fn new_zst(repr_align: Option<NonZeroUsize>) -> DstLayout {
        let align = match repr_align {
            Some(align) => align,
            None => Self::MIN_ALIGN,
        };

        const_assert!(align.get().is_power_of_two());

        DstLayout {
            align,
            size_info: SizeInfo::Sized { size: 0 },
            statically_shallow_unpadded: true,
        }
    }

    /// Constructs a `DstLayout` which describes `T` and assumes `T` may contain
    /// padding.
    ///
    /// # Safety
    ///
    /// Unsafe code may assume that `DstLayout` is the correct layout for `T`.
    #[doc(hidden)]
    #[must_use]
    #[inline]
    pub const fn for_type<T>() -> DstLayout {
        DstLayout {
            align: match NonZeroUsize::new(mem::align_of::<T>()) {
                Some(align) => align,
                None => const_unreachable!(),
            },
            size_info: SizeInfo::Sized { size: mem::size_of::<T>() },
            statically_shallow_unpadded: false,
        }
    }

    /// Constructs a `DstLayout` which describes a `T` that does not contain
    /// padding.
    ///
    /// # Safety
    ///
    /// Unsafe code may assume that `DstLayout` is the correct layout for `T`.
    #[doc(hidden)]
    #[must_use]
    #[inline]
    pub const fn for_unpadded_type<T>() -> DstLayout {
        Self::for_type::<T>().assume_shallow_unpadded()
    }

    /// Constructs a `DstLayout` which describes `[T]`.
    ///
    /// # Safety
    ///
    /// Unsafe code may assume that `DstLayout` is the correct layout for `[T]`.
    pub(crate) const fn for_slice<T>() -> DstLayout {
        DstLayout {
            align: match NonZeroUsize::new(mem::align_of::<T>()) {
                Some(align) => align,
                None => const_unreachable!(),
            },
            size_info: SizeInfo::SliceDst(TrailingSliceLayout {
                offset: 0,
                elem_size: mem::size_of::<T>(),
            }),
            statically_shallow_unpadded: true,
        }
    }

    /// Constructs a complete `DstLayout` reflecting a `repr(C)` struct with the
    /// given alignment modifiers and fields.
    ///
    /// This method cannot be used to match the layout of a record with the
    /// default representation, as that representation is mostly unspecified.
    ///
    /// # Safety
    ///
    /// For any definition of a `repr(C)` struct, if this method is invoked with
    /// alignment modifiers and fields corresponding to that definition, the
    /// resulting `DstLayout` will correctly encode the layout of that struct.
    ///
    /// We make no guarantees to the behavior of this method when it is invoked
    /// with arguments that cannot correspond to a valid `repr(C)` struct.
    #[must_use]
    #[inline]
    pub const fn for_repr_c_struct(
        repr_align: Option<NonZeroUsize>,
        repr_packed: Option<NonZeroUsize>,
        fields: &[DstLayout],
    ) -> DstLayout {
        let mut layout = DstLayout::new_zst(repr_align);

        let mut i = 0;
        #[allow(clippy::arithmetic_side_effects)]
        while i < fields.len() {
            #[allow(clippy::indexing_slicing)]
            let field = fields[i];
            layout = layout.extend(field, repr_packed);
            i += 1;
        }

        layout = layout.pad_to_align();

        layout
    }

    /// Like `Layout::extend`, this creates a layout that describes a record
    /// whose layout consists of `self` followed by `next` that includes the
    /// necessary inter-field padding, but not any trailing padding.
    ///
    /// In order to match the layout of a `#[repr(C)]` struct, this method
    /// should be invoked for each field in declaration order. To add trailing
    /// padding, call `DstLayout::pad_to_align` after extending the layout for
    /// all fields. If `self` corresponds to a type marked with
    /// `repr(packed(N))`, then `repr_packed` should be set to `Some(N)`,
    /// otherwise `None`.
    ///
    /// This method cannot be used to match the layout of a record with the
    /// default representation, as that representation is mostly unspecified.
    ///
    /// # Safety
    ///
    /// If a (potentially hypothetical) valid `repr(C)` Rust type begins with
    /// fields whose layout are `self`, and those fields are immediately
    /// followed by a field whose layout is `field`, then unsafe code may rely
    /// on `self.extend(field, repr_packed)` producing a layout that correctly
    /// encompasses those two components.
    ///
    /// We make no guarantees to the behavior of this method if these fragments
    /// cannot appear in a valid Rust type (e.g., the concatenation of the
    /// layouts would lead to a size larger than `isize::MAX`).
    #[doc(hidden)]
    #[must_use]
    #[inline]
    pub const fn extend(self, field: DstLayout, repr_packed: Option<NonZeroUsize>) -> Self {
        use util::{max, min, padding_needed_for};

        let max_align = match repr_packed {
            Some(max_align) => max_align,
            None => Self::THEORETICAL_MAX_ALIGN,
        };

        const_assert!(max_align.get().is_power_of_two());

        #[cfg(not(kani))]
        {
            const_debug_assert!(self.align.get() <= DstLayout::CURRENT_MAX_ALIGN.get());
            const_debug_assert!(field.align.get() <= DstLayout::CURRENT_MAX_ALIGN.get());
            if let Some(repr_packed) = repr_packed {
                const_debug_assert!(repr_packed.get() <= DstLayout::CURRENT_MAX_ALIGN.get());
            }
        }

        let field_align = min(field.align, max_align);

        let align = max(self.align, field_align);

        let (interfield_padding, size_info) = match self.size_info {
            SizeInfo::SliceDst(..) => const_panic!("Cannot extend a DST with additional fields."),

            SizeInfo::Sized { size: preceding_size } => {
                let padding = padding_needed_for(preceding_size, field_align);

                let offset = match preceding_size.checked_add(padding) {
                    Some(offset) => offset,
                    None => const_panic!("Adding padding to `self`'s size overflows `usize`."),
                };

                (
                    padding,
                    match field.size_info {
                        SizeInfo::Sized { size: field_size } => {
                            let size = match offset.checked_add(field_size) {
                                Some(size) => size,
                                None => const_panic!("`field` cannot be appended without the total size overflowing `usize`"),
                            };
                            SizeInfo::Sized { size }
                        }
                        SizeInfo::SliceDst(TrailingSliceLayout {
                            offset: trailing_offset,
                            elem_size,
                        }) => {
                            let offset = match offset.checked_add(trailing_offset) {
                                Some(offset) => offset,
                                None => const_panic!("`field` cannot be appended without the total size overflowing `usize`"),
                            };
                            SizeInfo::SliceDst(TrailingSliceLayout { offset, elem_size })
                        }
                    },
                )
            }
        };

        let statically_shallow_unpadded = self.statically_shallow_unpadded
            && field.statically_shallow_unpadded
            && interfield_padding == 0;

        DstLayout { align, size_info, statically_shallow_unpadded }
    }

    /// Like `Layout::pad_to_align`, this routine rounds the size of this layout
    /// up to the nearest multiple of this type's alignment or `repr_packed`
    /// (whichever is less). This method leaves DST layouts unchanged, since the
    /// trailing padding of DSTs is computed at runtime.
    ///
    /// The accompanying boolean is `true` if the resulting composition of
    /// fields necessitated static (as opposed to dynamic) padding; otherwise
    /// `false`.
    ///
    /// In order to match the layout of a `#[repr(C)]` struct, this method
    /// should be invoked after the invocations of [`DstLayout::extend`]. If
    /// `self` corresponds to a type marked with `repr(packed(N))`, then
    /// `repr_packed` should be set to `Some(N)`, otherwise `None`.
    ///
    /// This method cannot be used to match the layout of a record with the
    /// default representation, as that representation is mostly unspecified.
    ///
    /// # Safety
    ///
    /// If a (potentially hypothetical) valid `repr(C)` type begins with fields
    /// whose layout are `self` followed only by zero or more bytes of trailing
    /// padding (not included in `self`), then unsafe code may rely on
    /// `self.pad_to_align(repr_packed)` producing a layout that correctly
    /// encapsulates the layout of that type.
    ///
    /// We make no guarantees to the behavior of this method if `self` cannot
    /// appear in a valid Rust type (e.g., because the addition of trailing
    /// padding would lead to a size larger than `isize::MAX`).
    #[doc(hidden)]
    #[must_use]
    #[inline]
    pub const fn pad_to_align(self) -> Self {
        use util::padding_needed_for;

        let (static_padding, size_info) = match self.size_info {
            SizeInfo::Sized { size: unpadded_size } => {
                let padding = padding_needed_for(unpadded_size, self.align);
                let size = match unpadded_size.checked_add(padding) {
                    Some(size) => size,
                    None => const_panic!("Adding padding caused size to overflow `usize`."),
                };
                (padding, SizeInfo::Sized { size })
            }
            size_info @ SizeInfo::SliceDst(_) => (0, size_info),
        };

        let statically_shallow_unpadded = self.statically_shallow_unpadded && static_padding == 0;

        DstLayout { align: self.align, size_info, statically_shallow_unpadded }
    }

    /// Produces `true` if `self` requires static padding; otherwise `false`.
    #[must_use]
    #[inline(always)]
    pub const fn requires_static_padding(self) -> bool {
        !self.statically_shallow_unpadded
    }

    /// Produces `true` if there exists any metadata for which a type of layout
    /// `self` would require dynamic trailing padding; otherwise `false`.
    #[must_use]
    #[inline(always)]
    pub const fn requires_dynamic_padding(self) -> bool {
        #[allow(clippy::arithmetic_side_effects)]
        match self.size_info {
            SizeInfo::Sized { .. } => false,
            SizeInfo::SliceDst(trailing_slice_layout) => {
                trailing_slice_layout.offset % self.align.get() != 0
                    || trailing_slice_layout.elem_size % self.align.get() != 0
            }
        }
    }

    /// Validates that a cast is sound from a layout perspective.
    ///
    /// Validates that the size and alignment requirements of a type with the
    /// layout described in `self` would not be violated by performing a
    /// `cast_type` cast from a pointer with address `addr` which refers to a
    /// memory region of size `bytes_len`.
    ///
    /// If the cast is valid, `validate_cast_and_convert_metadata` returns
    /// `(elems, split_at)`. If `self` describes a dynamically-sized type, then
    /// `elems` is the maximum number of trailing slice elements for which a
    /// cast would be valid (for sized types, `elem` is meaningless and should
    /// be ignored). `split_at` is the index at which to split the memory region
    /// in order for the prefix (suffix) to contain the result of the cast, and
    /// in order for the remaining suffix (prefix) to contain the leftover
    /// bytes.
    ///
    /// There are three conditions under which a cast can fail:
    /// - The smallest possible value for the type is larger than the provided
    ///   memory region
    /// - A prefix cast is requested, and `addr` does not satisfy `self`'s
    ///   alignment requirement
    /// - A suffix cast is requested, and `addr + bytes_len` does not satisfy
    ///   `self`'s alignment requirement (as a consequence, since all instances
    ///   of the type are a multiple of its alignment, no size for the type will
    ///   result in a starting address which is properly aligned)
    ///
    /// # Safety
    ///
    /// The caller may assume that this implementation is correct, and may rely
    /// on that assumption for the soundness of their code. In particular, the
    /// caller may assume that, if `validate_cast_and_convert_metadata` returns
    /// `Some((elems, split_at))`, then:
    /// - A pointer to the type (for dynamically sized types, this includes
    ///   `elems` as its pointer metadata) describes an object of size `size <=
    ///   bytes_len`
    /// - If this is a prefix cast:
    ///   - `addr` satisfies `self`'s alignment
    ///   - `size == split_at`
    /// - If this is a suffix cast:
    ///   - `split_at == bytes_len - size`
    ///   - `addr + split_at` satisfies `self`'s alignment
    ///
    /// Note that this method does *not* ensure that a pointer constructed from
    /// its return values will be a valid pointer. In particular, this method
    /// does not reason about `isize` overflow, which is a requirement of many
    /// Rust pointer APIs, and may at some point be determined to be a validity
    /// invariant of pointer types themselves. This should never be a problem so
    /// long as the arguments to this method are derived from a known-valid
    /// pointer (e.g., one derived from a safe Rust reference), but it is
    /// nonetheless the caller's responsibility to justify that pointer
    /// arithmetic will not overflow based on a safety argument *other than* the
    /// mere fact that this method returned successfully.
    ///
    /// # Panics
    ///
    /// `validate_cast_and_convert_metadata` will panic if `self` describes a
    /// DST whose trailing slice element is zero-sized.
    ///
    /// If `addr + bytes_len` overflows `usize`,
    /// `validate_cast_and_convert_metadata` may panic, or it may return
    /// incorrect results. No guarantees are made about when
    /// `validate_cast_and_convert_metadata` will panic. The caller should not
    /// rely on `validate_cast_and_convert_metadata` panicking in any particular
    /// condition, even if `debug_assertions` are enabled.
    #[allow(unused)]
    #[inline(always)]
    pub(crate) const fn validate_cast_and_convert_metadata(
        &self,
        addr: usize,
        bytes_len: usize,
        cast_type: CastType,
    ) -> Result<(usize, usize), MetadataCastError> {
        // `debug_assert!`, but with `#[allow(clippy::arithmetic_side_effects)]`.
        macro_rules! __const_debug_assert {
            ($e:expr $(, $msg:expr)?) => {
                const_debug_assert!({
                    #[allow(clippy::arithmetic_side_effects)]
                    let e = $e;
                    e
                } $(, $msg)?);
            };
        }

        let size_info = match self.size_info.try_to_nonzero_elem_size() {
            Some(size_info) => size_info,
            None => const_panic!("attempted to cast to slice type with zero-sized element"),
        };

        __const_debug_assert!(
            addr.checked_add(bytes_len).is_some(),
            "`addr` + `bytes_len` > usize::MAX"
        );

        {
            let offset = match cast_type {
                CastType::Prefix => 0,
                CastType::Suffix => bytes_len,
            };

            #[allow(clippy::arithmetic_side_effects)]
            if (addr + offset) % self.align.get() != 0 {
                return Err(MetadataCastError::Alignment);
            }
        }

        let (elems, self_bytes) = match size_info {
            SizeInfo::Sized { size } => {
                if size > bytes_len {
                    return Err(MetadataCastError::Size);
                }
                (0, size)
            }
            SizeInfo::SliceDst(TrailingSliceLayout { offset, elem_size }) => {
                let max_total_bytes =
                    util::round_down_to_next_multiple_of_alignment(bytes_len, self.align);
                let max_slice_and_padding_bytes = match max_total_bytes.checked_sub(offset) {
                    Some(max) => max,
                    None => return Err(MetadataCastError::Size),
                };

                #[allow(clippy::arithmetic_side_effects)]
                let elems = max_slice_and_padding_bytes / elem_size.get();
                #[allow(clippy::arithmetic_side_effects)]
                let without_padding = offset + elems * elem_size.get();
                #[allow(clippy::arithmetic_side_effects)]
                let self_bytes =
                    without_padding + util::padding_needed_for(without_padding, self.align);
                (elems, self_bytes)
            }
        };

        __const_debug_assert!(self_bytes <= bytes_len);

        let split_at = match cast_type {
            CastType::Prefix => self_bytes,
            #[allow(clippy::arithmetic_side_effects)]
            CastType::Suffix => bytes_len - self_bytes,
        };

        Ok((elems, split_at))
    }
}

pub(crate) use cast_from_raw::cast_from_raw;
mod cast_from_raw {
    use crate::{pointer::PtrInner, *};

    /// Implements [`<Dst as SizeEq<Src>>::cast_from_raw`][cast_from_raw].
    ///
    /// # PME
    ///
    /// Generates a post-monomorphization error if it is not possible to satisfy
    /// the soundness conditions of [`SizeEq::cast_from_raw`][cast_from_raw]
    /// for `Src` and `Dst`.
    ///
    /// [cast_from_raw]: crate::pointer::SizeEq::cast_from_raw
    pub(crate) fn cast_from_raw<Src, Dst>(src: PtrInner<'_, Src>) -> PtrInner<'_, Dst>
    where
        Src: KnownLayout<PointerMetadata = usize> + ?Sized,
        Dst: KnownLayout<PointerMetadata = usize> + ?Sized,
    {

        /// The parameters required in order to perform a pointer cast from
        /// `Src` to `Dst` as described above.
        ///
        /// These are a compile-time function of the layouts of `Src` and `Dst`.
        ///
        /// # Safety
        ///
        /// `offset_delta_elems` and `elem_multiple` must be valid as described
        /// above.
        ///
        /// `Src`'s alignment must not be smaller than `Dst`'s alignment.
        #[derive(Copy, Clone)]
        struct CastParams {
            offset_delta_elems: usize,
            elem_multiple: usize,
        }

        impl CastParams {
            const fn try_compute(src: &DstLayout, dst: &DstLayout) -> Option<CastParams> {
                if src.align.get() < dst.align.get() {
                    return None;
                }

                let (src, dst) = if let (SizeInfo::SliceDst(src), SizeInfo::SliceDst(dst)) =
                    (src.size_info, dst.size_info)
                {
                    (src, dst)
                } else {
                    return None;
                };

                let offset_delta = if let Some(od) = src.offset.checked_sub(dst.offset) {
                    od
                } else {
                    return None;
                };

                let dst_elem_size = if let Some(e) = NonZeroUsize::new(dst.elem_size) {
                    e
                } else {
                    return None;
                };

                #[allow(clippy::arithmetic_side_effects)]
                let delta_mod_other_elem = offset_delta % dst_elem_size.get();

                #[allow(clippy::arithmetic_side_effects)]
                let elem_remainder = src.elem_size % dst_elem_size.get();

                if delta_mod_other_elem != 0 || src.elem_size < dst.elem_size || elem_remainder != 0
                {
                    return None;
                }

                #[allow(clippy::arithmetic_side_effects)]
                let offset_delta_elems = offset_delta / dst_elem_size.get();

                #[allow(clippy::arithmetic_side_effects)]
                let elem_multiple = src.elem_size / dst_elem_size.get();

                Some(CastParams {
                    offset_delta_elems,
                    elem_multiple,
                })
            }

            /// # Safety
            ///
            /// `src_meta` describes a `Src` whose size is no larger than
            /// `isize::MAX`.
            ///
            /// The returned metadata describes a `Dst` of the same size as the
            /// original `Src`.
            unsafe fn cast_metadata(self, src_meta: usize) -> usize {
                #[allow(unused)]
                use crate::util::polyfills::*;

                #[allow(unstable_name_collisions)]
                unsafe {
                    self.offset_delta_elems
                        .unchecked_add(src_meta.unchecked_mul(self.elem_multiple))
                }
            }
        }

        trait Params<Src: ?Sized> {
            const CAST_PARAMS: CastParams;
        }

        impl<Src, Dst> Params<Src> for Dst
        where
            Src: KnownLayout + ?Sized,
            Dst: KnownLayout<PointerMetadata = usize> + ?Sized,
        {
            const CAST_PARAMS: CastParams =
                match CastParams::try_compute(&Src::LAYOUT, &Dst::LAYOUT) {
                    Some(params) => params,
                    None => const_panic!(
                        "cannot `transmute_ref!` or `transmute_mut!` between incompatible types"
                    ),
                };
        }

        let src_meta = <Src as KnownLayout>::pointer_to_metadata(src.as_non_null().as_ptr());
        let params = <Dst as Params<Src>>::CAST_PARAMS;

        let dst_meta = unsafe { params.cast_metadata(src_meta) };

        let dst = <Dst as KnownLayout>::raw_from_ptr_len(src.as_non_null().cast(), dst_meta);

        unsafe { PtrInner::new(dst) }
    }
}


#[cfg(kani)]
mod proofs {
    use core::alloc::Layout;

    use super::*;

    impl kani::Arbitrary for DstLayout {
        fn any() -> Self {
            let align: NonZeroUsize = kani::any();
            let size_info: SizeInfo = kani::any();

            kani::assume(align.is_power_of_two());
            kani::assume(align < DstLayout::THEORETICAL_MAX_ALIGN);

            kani::assume(
                match size_info {
                    SizeInfo::Sized { size } => Layout::from_size_align(size, align.get()),
                    SizeInfo::SliceDst(TrailingSliceLayout { offset, elem_size: _ }) => {
                        Layout::from_size_align(offset, align.get())
                    }
                }
                .is_ok(),
            );

            Self { align: align, size_info: size_info, statically_shallow_unpadded: kani::any() }
        }
    }

    impl kani::Arbitrary for SizeInfo {
        fn any() -> Self {
            let is_sized: bool = kani::any();

            match is_sized {
                true => {
                    let size: usize = kani::any();

                    kani::assume(size <= isize::MAX as _);

                    SizeInfo::Sized { size }
                }
                false => SizeInfo::SliceDst(kani::any()),
            }
        }
    }

    impl kani::Arbitrary for TrailingSliceLayout {
        fn any() -> Self {
            let elem_size: usize = kani::any();
            let offset: usize = kani::any();

            kani::assume(elem_size < isize::MAX as _);
            kani::assume(offset < isize::MAX as _);

            TrailingSliceLayout { elem_size, offset }
        }
    }

    #[kani::proof]
    fn prove_requires_dynamic_padding() {
        let layout: DstLayout = kani::any();

        let SizeInfo::SliceDst(size_info) = layout.size_info else {
            kani::assume(false);
            loop {}
        };

        let meta: usize = kani::any();

        let Some(trailing_slice_size) = size_info.elem_size.checked_mul(meta) else {
            kani::assume(false);
            loop {}
        };

        let Some(unpadded_size) = size_info.offset.checked_add(trailing_slice_size) else {
            kani::assume(false);
            loop {}
        };

        if unpadded_size >= isize::MAX as usize {
            kani::assume(false);
            loop {}
        }

        let trailing_padding = util::padding_needed_for(unpadded_size, layout.align);

        if !layout.requires_dynamic_padding() {
            assert!(trailing_padding == 0);
        }
    }

    #[kani::proof]
    fn prove_dst_layout_extend() {
        use crate::util::{max, min, padding_needed_for};

        let base: DstLayout = kani::any();
        let field: DstLayout = kani::any();
        let packed: Option<NonZeroUsize> = kani::any();

        if let Some(max_align) = packed {
            kani::assume(max_align.is_power_of_two());
            kani::assume(base.align <= max_align);
        }

        kani::assume(matches!(base.size_info, SizeInfo::Sized { .. }));
        let base_size = if let SizeInfo::Sized { size } = base.size_info {
            size
        } else {
            unreachable!();
        };

        let composite = base.extend(field, packed);

        let field_align = min(field.align, packed.unwrap_or(DstLayout::THEORETICAL_MAX_ALIGN));

        assert_eq!(composite.align, max(base.align, field_align));

        let padding = padding_needed_for(base_size, field_align);
        let offset = base_size + padding;

        let base_analog = Layout::from_size_align(base_size, base.align.get()).unwrap();

        match field.size_info {
            SizeInfo::Sized { size: field_size } => {
                if let SizeInfo::Sized { size: composite_size } = composite.size_info {
                    assert_eq!(composite_size, offset + field_size);

                    let field_analog =
                        Layout::from_size_align(field_size, field_align.get()).unwrap();

                    if let Ok((actual_composite, actual_offset)) = base_analog.extend(field_analog)
                    {
                        assert_eq!(actual_offset, offset);
                        assert_eq!(actual_composite.size(), composite_size);
                        assert_eq!(actual_composite.align(), composite.align.get());
                    } else {
                    }
                } else {
                    panic!("The composite of two sized layouts must be sized.")
                }
            }
            SizeInfo::SliceDst(TrailingSliceLayout {
                offset: field_offset,
                elem_size: field_elem_size,
            }) => {
                if let SizeInfo::SliceDst(TrailingSliceLayout {
                    offset: composite_offset,
                    elem_size: composite_elem_size,
                }) = composite.size_info
                {
                    assert_eq!(composite_offset, offset + field_offset);
                    assert_eq!(composite_elem_size, field_elem_size);

                    let field_analog =
                        Layout::from_size_align(field_offset, field_align.get()).unwrap();

                    if let Ok((actual_composite, actual_offset)) = base_analog.extend(field_analog)
                    {
                        assert_eq!(actual_offset, offset);
                        assert_eq!(actual_composite.size(), composite_offset);
                        assert_eq!(actual_composite.align(), composite.align.get());
                    } else {
                    }
                } else {
                    panic!("The extension of a layout with a DST must result in a DST.")
                }
            }
        }
    }

    #[kani::proof]
    #[kani::should_panic]
    fn prove_dst_layout_extend_dst_panics() {
        let base: DstLayout = kani::any();
        let field: DstLayout = kani::any();
        let packed: Option<NonZeroUsize> = kani::any();

        if let Some(max_align) = packed {
            kani::assume(max_align.is_power_of_two());
            kani::assume(base.align <= max_align);
        }

        kani::assume(matches!(base.size_info, SizeInfo::SliceDst(..)));

        let _ = base.extend(field, packed);
    }

    #[kani::proof]
    fn prove_dst_layout_pad_to_align() {
        use crate::util::padding_needed_for;

        let layout: DstLayout = kani::any();

        let padded = layout.pad_to_align();

        assert_eq!(padded.align, layout.align);

        if let SizeInfo::Sized { size: unpadded_size } = layout.size_info {
            if let SizeInfo::Sized { size: padded_size } = padded.size_info {
                let padding = padding_needed_for(unpadded_size, layout.align);
                assert_eq!(padded_size, unpadded_size + padding);

                let layout_analog =
                    Layout::from_size_align(unpadded_size, layout.align.get()).unwrap();
                let padded_analog = layout_analog.pad_to_align();
                assert_eq!(padded_analog.align(), layout.align.get());
                assert_eq!(padded_analog.size(), padded_size);
            } else {
                panic!("The padding of a sized layout must result in a sized layout.")
            }
        } else {
            assert_eq!(padded.size_info, layout.size_info);
        }
    }
}
