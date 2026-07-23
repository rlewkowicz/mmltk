// Copyright 2024 The Fuchsia Authors
// Licensed under the 2-Clause BSD License <LICENSE-BSD or
// https://opensource.org/license/bsd-2-clause>, Apache License, Version 2.0
// <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
// license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.

use super::*;

mod def {
    use core::marker::PhantomData;

    use crate::{
        ByteSlice, ByteSliceMut, CloneableByteSlice, CopyableByteSlice, IntoByteSlice,
        IntoByteSliceMut,
    };

    /// A typed reference derived from a byte slice.
    ///
    /// A `Ref<B, T>` is a reference to a `T` which is stored in a byte slice, `B`.
    /// Unlike a native reference (`&T` or `&mut T`), `Ref<B, T>` has the same
    /// mutability as the byte slice it was constructed from (`B`).
    ///
    /// # Examples
    ///
    /// `Ref` can be used to treat a sequence of bytes as a structured type, and
    /// to read and write the fields of that type as if the byte slice reference
    /// were simply a reference to that type.
    ///
    /// ```rust
    /// use zerocopy::*;
    /// # use zerocopy_derive::*;
    ///
    /// #[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned)]
    /// #[repr(C)]
    /// struct UdpHeader {
    ///     src_port: [u8; 2],
    ///     dst_port: [u8; 2],
    ///     length: [u8; 2],
    ///     checksum: [u8; 2],
    /// }
    ///
    /// #[derive(FromBytes, IntoBytes, KnownLayout, Immutable, Unaligned)]
    /// #[repr(C, packed)]
    /// struct UdpPacket {
    ///     header: UdpHeader,
    ///     body: [u8],
    /// }
    ///
    /// impl UdpPacket {
    ///     pub fn parse<B: ByteSlice>(bytes: B) -> Option<Ref<B, UdpPacket>> {
    ///         Ref::from_bytes(bytes).ok()
    ///     }
    /// }
    /// ```
    pub struct Ref<B, T: ?Sized>(
        B,
        PhantomData<T>,
    );

    impl<B, T: ?Sized> Ref<B, T> {
        /// Constructs a new `Ref`.
        ///
        /// # Safety
        ///
        /// `bytes` dereferences (via [`deref`], [`deref_mut`], and [`into`]) to
        /// a byte slice which is aligned to `T`'s alignment and whose size is a
        /// valid size for `T`.
        ///
        /// [`deref`]: core::ops::Deref::deref
        /// [`deref_mut`]: core::ops::DerefMut::deref_mut
        /// [`into`]: core::convert::Into::into
        pub(crate) unsafe fn new_unchecked(bytes: B) -> Ref<B, T> {
            Ref(bytes, PhantomData)
        }
    }

    impl<B: ByteSlice, T: ?Sized> Ref<B, T> {
        /// Access the byte slice as a [`ByteSlice`].
        ///
        /// # Safety
        ///
        /// The caller promises not to call methods on the returned
        /// [`ByteSlice`] other than `ByteSlice` methods (for example, via
        /// `Any::downcast_ref`).
        ///
        /// `as_byte_slice` promises to return a `ByteSlice` whose referent is
        /// validly-aligned for `T` and has a valid size for `T`.
        pub(crate) unsafe fn as_byte_slice(&self) -> &impl ByteSlice {
            &self.0
        }
    }

    impl<B: ByteSliceMut, T: ?Sized> Ref<B, T> {
        /// Access the byte slice as a [`ByteSliceMut`].
        ///
        /// # Safety
        ///
        /// The caller promises not to call methods on the returned
        /// [`ByteSliceMut`] other than `ByteSliceMut` methods (for example, via
        /// `Any::downcast_mut`).
        ///
        /// `as_byte_slice` promises to return a `ByteSlice` whose referent is
        /// validly-aligned for `T` and has a valid size for `T`.
        pub(crate) unsafe fn as_byte_slice_mut(&mut self) -> &mut impl ByteSliceMut {
            &mut self.0
        }
    }

    impl<'a, B: IntoByteSlice<'a>, T: ?Sized> Ref<B, T> {
        /// Access the byte slice as an [`IntoByteSlice`].
        ///
        /// # Safety
        ///
        /// The caller promises not to call methods on the returned
        /// [`IntoByteSlice`] other than `IntoByteSlice` methods (for example,
        /// via `Any::downcast_ref`).
        ///
        /// `as_byte_slice` promises to return a `ByteSlice` whose referent is
        /// validly-aligned for `T` and has a valid size for `T`.
        pub(crate) unsafe fn into_byte_slice(self) -> impl IntoByteSlice<'a> {
            self.0
        }
    }

    impl<'a, B: IntoByteSliceMut<'a>, T: ?Sized> Ref<B, T> {
        /// Access the byte slice as an [`IntoByteSliceMut`].
        ///
        /// # Safety
        ///
        /// The caller promises not to call methods on the returned
        /// [`IntoByteSliceMut`] other than `IntoByteSliceMut` methods (for
        /// example, via `Any::downcast_mut`).
        ///
        /// `as_byte_slice` promises to return a `ByteSlice` whose referent is
        /// validly-aligned for `T` and has a valid size for `T`.
        pub(crate) unsafe fn into_byte_slice_mut(self) -> impl IntoByteSliceMut<'a> {
            self.0
        }
    }

    impl<B: CloneableByteSlice + Clone, T: ?Sized> Clone for Ref<B, T> {
        #[inline]
        fn clone(&self) -> Ref<B, T> {
            Ref(self.0.clone(), PhantomData)
        }
    }

    impl<B: CopyableByteSlice + Copy, T: ?Sized> Copy for Ref<B, T> {}
}

#[allow(unreachable_pub)] 
pub use def::Ref;

impl<B, T> Ref<B, T>
where
    B: ByteSlice,
{
    #[must_use = "has no side effects"]
    pub(crate) fn sized_from(bytes: B) -> Result<Ref<B, T>, CastError<B, T>> {
        if bytes.len() != mem::size_of::<T>() {
            return Err(SizeError::new(bytes).into());
        }
        if let Err(err) = util::validate_aligned_to::<_, T>(bytes.deref()) {
            return Err(err.with_src(bytes).into());
        }

        Ok(unsafe { Ref::new_unchecked(bytes) })
    }
}

impl<B, T> Ref<B, T>
where
    B: SplitByteSlice,
{
    #[must_use = "has no side effects"]
    pub(crate) fn sized_from_prefix(bytes: B) -> Result<(Ref<B, T>, B), CastError<B, T>> {
        if bytes.len() < mem::size_of::<T>() {
            return Err(SizeError::new(bytes).into());
        }
        if let Err(err) = util::validate_aligned_to::<_, T>(bytes.deref()) {
            return Err(err.with_src(bytes).into());
        }
        let (bytes, suffix) =
            bytes.split_at(mem::size_of::<T>()).map_err(|b| SizeError::new(b).into())?;
        let r = unsafe { Ref::new_unchecked(bytes) };
        Ok((r, suffix))
    }

    #[must_use = "has no side effects"]
    pub(crate) fn sized_from_suffix(bytes: B) -> Result<(B, Ref<B, T>), CastError<B, T>> {
        let bytes_len = bytes.len();
        let split_at = if let Some(split_at) = bytes_len.checked_sub(mem::size_of::<T>()) {
            split_at
        } else {
            return Err(SizeError::new(bytes).into());
        };
        let (prefix, bytes) = bytes.split_at(split_at).map_err(|b| SizeError::new(b).into())?;
        if let Err(err) = util::validate_aligned_to::<_, T>(bytes.deref()) {
            return Err(err.with_src(bytes).into());
        }
        let r = unsafe { Ref::new_unchecked(bytes) };
        Ok((prefix, r))
    }
}

impl<B, T> Ref<B, T>
where
    B: ByteSlice,
    T: KnownLayout + Immutable + ?Sized,
{
    /// Constructs a `Ref` from a byte slice.
    ///
    /// If the length of `source` is not a [valid size of `T`][valid-size], or
    /// if `source` is not appropriately aligned for `T`, this returns `Err`. If
    /// [`T: Unaligned`][t-unaligned], you can [infallibly discard the alignment
    /// error][size-error-from].
    ///
    /// `T` may be a sized type, a slice, or a [slice DST][slice-dst].
    ///
    /// [valid-size]: crate::KnownLayout#what-is-a-valid-size
    /// [t-unaligned]: Unaligned
    /// [size-error-from]: error/struct.SizeError.html#method.from-1
    /// [slice-dst]: KnownLayout#dynamically-sized-types
    ///
    /// # Compile-Time Assertions
    ///
    /// This method cannot yet be used on unsized types whose dynamically-sized
    /// component is zero-sized. Attempting to use this method on such types
    /// results in a compile-time assertion error; e.g.:
    ///
    /// ```compile_fail,E0080
    /// use zerocopy::*;
    /// # use zerocopy_derive::*;
    ///
    /// #[derive(Immutable, KnownLayout)]
    /// #[repr(C)]
    /// struct ZSTy {
    ///     leading_sized: u16,
    ///     trailing_dst: [()],
    /// }
    ///
    /// let _ = Ref::<_, ZSTy>::from_bytes(&b"UU"[..]); // ⚠ Compile Error!
    /// ```
    #[must_use = "has no side effects"]
    #[inline]
    pub fn from_bytes(source: B) -> Result<Ref<B, T>, CastError<B, T>> {
        static_assert_dst_is_not_zst!(T);
        if let Err(e) =
            Ptr::from_ref(source.deref()).try_cast_into_no_leftover::<T, BecauseImmutable>(None)
        {
            return Err(e.with_src(()).with_src(source));
        }
        Ok(unsafe { Ref::new_unchecked(source) })
    }
}

impl<B, T> Ref<B, T>
where
    B: SplitByteSlice,
    T: KnownLayout + Immutable + ?Sized,
{
    /// Constructs a `Ref` from the prefix of a byte slice.
    ///
    /// This method computes the [largest possible size of `T`][valid-size] that
    /// can fit in the leading bytes of `source`, then attempts to return both a
    /// `Ref` to those bytes, and a reference to the remaining bytes. If there
    /// are insufficient bytes, or if `source` is not appropriately aligned,
    /// this returns `Err`. If [`T: Unaligned`][t-unaligned], you can
    /// [infallibly discard the alignment error][size-error-from].
    ///
    /// `T` may be a sized type, a slice, or a [slice DST][slice-dst].
    ///
    /// [valid-size]: crate::KnownLayout#what-is-a-valid-size
    /// [t-unaligned]: Unaligned
    /// [size-error-from]: error/struct.SizeError.html#method.from-1
    /// [slice-dst]: KnownLayout#dynamically-sized-types
    ///
    /// # Compile-Time Assertions
    ///
    /// This method cannot yet be used on unsized types whose dynamically-sized
    /// component is zero-sized. Attempting to use this method on such types
    /// results in a compile-time assertion error; e.g.:
    ///
    /// ```compile_fail,E0080
    /// use zerocopy::*;
    /// # use zerocopy_derive::*;
    ///
    /// #[derive(Immutable, KnownLayout)]
    /// #[repr(C)]
    /// struct ZSTy {
    ///     leading_sized: u16,
    ///     trailing_dst: [()],
    /// }
    ///
    /// let _ = Ref::<_, ZSTy>::from_prefix(&b"UU"[..]); // ⚠ Compile Error!
    /// ```
    #[must_use = "has no side effects"]
    #[inline]
    pub fn from_prefix(source: B) -> Result<(Ref<B, T>, B), CastError<B, T>> {
        static_assert_dst_is_not_zst!(T);
        let remainder = match Ptr::from_ref(source.deref())
            .try_cast_into::<T, BecauseImmutable>(CastType::Prefix, None)
        {
            Ok((_, remainder)) => remainder,
            Err(e) => {
                return Err(e.with_src(()).with_src(source));
            }
        };

        #[allow(unstable_name_collisions)]
        let split_at = unsafe { source.len().unchecked_sub(remainder.len()) };
        let (bytes, suffix) = source.split_at(split_at).map_err(|b| SizeError::new(b).into())?;
        let r = unsafe { Ref::new_unchecked(bytes) };
        Ok((r, suffix))
    }

    /// Constructs a `Ref` from the suffix of a byte slice.
    ///
    /// This method computes the [largest possible size of `T`][valid-size] that
    /// can fit in the trailing bytes of `source`, then attempts to return both
    /// a `Ref` to those bytes, and a reference to the preceding bytes. If there
    /// are insufficient bytes, or if that suffix of `source` is not
    /// appropriately aligned, this returns `Err`. If [`T:
    /// Unaligned`][t-unaligned], you can [infallibly discard the alignment
    /// error][size-error-from].
    ///
    /// `T` may be a sized type, a slice, or a [slice DST][slice-dst].
    ///
    /// [valid-size]: crate::KnownLayout#what-is-a-valid-size
    /// [t-unaligned]: Unaligned
    /// [size-error-from]: error/struct.SizeError.html#method.from-1
    /// [slice-dst]: KnownLayout#dynamically-sized-types
    ///
    /// # Compile-Time Assertions
    ///
    /// This method cannot yet be used on unsized types whose dynamically-sized
    /// component is zero-sized. Attempting to use this method on such types
    /// results in a compile-time assertion error; e.g.:
    ///
    /// ```compile_fail,E0080
    /// use zerocopy::*;
    /// # use zerocopy_derive::*;
    ///
    /// #[derive(Immutable, KnownLayout)]
    /// #[repr(C)]
    /// struct ZSTy {
    ///     leading_sized: u16,
    ///     trailing_dst: [()],
    /// }
    ///
    /// let _ = Ref::<_, ZSTy>::from_suffix(&b"UU"[..]); // ⚠ Compile Error!
    /// ```
    #[must_use = "has no side effects"]
    #[inline]
    pub fn from_suffix(source: B) -> Result<(B, Ref<B, T>), CastError<B, T>> {
        static_assert_dst_is_not_zst!(T);
        let remainder = match Ptr::from_ref(source.deref())
            .try_cast_into::<T, BecauseImmutable>(CastType::Suffix, None)
        {
            Ok((_, remainder)) => remainder,
            Err(e) => {
                let e = e.with_src(());
                return Err(e.with_src(source));
            }
        };

        let split_at = remainder.len();
        let (prefix, bytes) = source.split_at(split_at).map_err(|b| SizeError::new(b).into())?;
        let r = unsafe { Ref::new_unchecked(bytes) };
        Ok((prefix, r))
    }
}

impl<B, T> Ref<B, T>
where
    B: ByteSlice,
    T: KnownLayout<PointerMetadata = usize> + Immutable + ?Sized,
{
    /// Constructs a `Ref` from the given bytes with DST length equal to `count`
    /// without copying.
    ///
    /// This method attempts to return a `Ref` to the prefix of `source`
    /// interpreted as a `T` with `count` trailing elements, and a reference to
    /// the remaining bytes. If the length of `source` is not equal to the size
    /// of `Self` with `count` elements, or if `source` is not appropriately
    /// aligned, this returns `Err`. If [`T: Unaligned`][t-unaligned], you can
    /// [infallibly discard the alignment error][size-error-from].
    ///
    /// [t-unaligned]: Unaligned
    /// [size-error-from]: error/struct.SizeError.html#method.from-1
    ///
    /// # Compile-Time Assertions
    ///
    /// This method cannot yet be used on unsized types whose dynamically-sized
    /// component is zero-sized. Attempting to use this method on such types
    /// results in a compile-time assertion error; e.g.:
    ///
    /// ```compile_fail,E0080
    /// use zerocopy::*;
    /// # use zerocopy_derive::*;
    ///
    /// #[derive(Immutable, KnownLayout)]
    /// #[repr(C)]
    /// struct ZSTy {
    ///     leading_sized: u16,
    ///     trailing_dst: [()],
    /// }
    ///
    /// let _ = Ref::<_, ZSTy>::from_bytes_with_elems(&b"UU"[..], 42); // ⚠ Compile Error!
    /// ```
    #[inline]
    pub fn from_bytes_with_elems(source: B, count: usize) -> Result<Ref<B, T>, CastError<B, T>> {
        static_assert_dst_is_not_zst!(T);
        let expected_len = match T::size_for_metadata(count) {
            Some(len) => len,
            None => return Err(SizeError::new(source).into()),
        };
        if source.len() != expected_len {
            return Err(SizeError::new(source).into());
        }
        Self::from_bytes(source)
    }
}

impl<B, T> Ref<B, T>
where
    B: SplitByteSlice,
    T: KnownLayout<PointerMetadata = usize> + Immutable + ?Sized,
{
    /// Constructs a `Ref` from the prefix of the given bytes with DST
    /// length equal to `count` without copying.
    ///
    /// This method attempts to return a `Ref` to the prefix of `source`
    /// interpreted as a `T` with `count` trailing elements, and a reference to
    /// the remaining bytes. If there are insufficient bytes, or if `source` is
    /// not appropriately aligned, this returns `Err`. If [`T:
    /// Unaligned`][t-unaligned], you can [infallibly discard the alignment
    /// error][size-error-from].
    ///
    /// [t-unaligned]: Unaligned
    /// [size-error-from]: error/struct.SizeError.html#method.from-1
    ///
    /// # Compile-Time Assertions
    ///
    /// This method cannot yet be used on unsized types whose dynamically-sized
    /// component is zero-sized. Attempting to use this method on such types
    /// results in a compile-time assertion error; e.g.:
    ///
    /// ```compile_fail,E0080
    /// use zerocopy::*;
    /// # use zerocopy_derive::*;
    ///
    /// #[derive(Immutable, KnownLayout)]
    /// #[repr(C)]
    /// struct ZSTy {
    ///     leading_sized: u16,
    ///     trailing_dst: [()],
    /// }
    ///
    /// let _ = Ref::<_, ZSTy>::from_prefix_with_elems(&b"UU"[..], 42); // ⚠ Compile Error!
    /// ```
    #[inline]
    pub fn from_prefix_with_elems(
        source: B,
        count: usize,
    ) -> Result<(Ref<B, T>, B), CastError<B, T>> {
        static_assert_dst_is_not_zst!(T);
        let expected_len = match T::size_for_metadata(count) {
            Some(len) => len,
            None => return Err(SizeError::new(source).into()),
        };
        let (prefix, bytes) = source.split_at(expected_len).map_err(SizeError::new)?;
        Self::from_bytes(prefix).map(move |l| (l, bytes))
    }

    /// Constructs a `Ref` from the suffix of the given bytes with DST length
    /// equal to `count` without copying.
    ///
    /// This method attempts to return a `Ref` to the suffix of `source`
    /// interpreted as a `T` with `count` trailing elements, and a reference to
    /// the preceding bytes. If there are insufficient bytes, or if that suffix
    /// of `source` is not appropriately aligned, this returns `Err`. If [`T:
    /// Unaligned`][t-unaligned], you can [infallibly discard the alignment
    /// error][size-error-from].
    ///
    /// [t-unaligned]: Unaligned
    /// [size-error-from]: error/struct.SizeError.html#method.from-1
    ///
    /// # Compile-Time Assertions
    ///
    /// This method cannot yet be used on unsized types whose dynamically-sized
    /// component is zero-sized. Attempting to use this method on such types
    /// results in a compile-time assertion error; e.g.:
    ///
    /// ```compile_fail,E0080
    /// use zerocopy::*;
    /// # use zerocopy_derive::*;
    ///
    /// #[derive(Immutable, KnownLayout)]
    /// #[repr(C)]
    /// struct ZSTy {
    ///     leading_sized: u16,
    ///     trailing_dst: [()],
    /// }
    ///
    /// let _ = Ref::<_, ZSTy>::from_suffix_with_elems(&b"UU"[..], 42); // ⚠ Compile Error!
    /// ```
    #[inline]
    pub fn from_suffix_with_elems(
        source: B,
        count: usize,
    ) -> Result<(B, Ref<B, T>), CastError<B, T>> {
        static_assert_dst_is_not_zst!(T);
        let expected_len = match T::size_for_metadata(count) {
            Some(len) => len,
            None => return Err(SizeError::new(source).into()),
        };
        let split_at = if let Some(split_at) = source.len().checked_sub(expected_len) {
            split_at
        } else {
            return Err(SizeError::new(source).into());
        };
        let (bytes, suffix) = unsafe { source.split_at_unchecked(split_at) };
        Self::from_bytes(suffix).map(move |l| (bytes, l))
    }
}

impl<'a, B, T> Ref<B, T>
where
    B: 'a + IntoByteSlice<'a>,
    T: FromBytes + KnownLayout + Immutable + ?Sized,
{
    /// Converts this `Ref` into a reference.
    ///
    /// `into_ref` consumes the `Ref`, and returns a reference to `T`.
    ///
    /// Note: this is an associated function, which means that you have to call
    /// it as `Ref::into_ref(r)` instead of `r.into_ref()`. This is so that
    /// there is no conflict with a method on the inner type.
    #[must_use = "has no side effects"]
    #[inline(always)]
    pub fn into_ref(r: Self) -> &'a T {
        static_assert_dst_is_not_zst!(T);

        let b = unsafe { r.into_byte_slice() };

        let ptr = Ptr::from_ref(b.into_byte_slice())
            .try_cast_into_no_leftover::<T, BecauseImmutable>(None)
            .expect("zerocopy internal error: into_ref should be infallible");
        let ptr = ptr.recall_validity();
        ptr.as_ref()
    }
}

impl<'a, B, T> Ref<B, T>
where
    B: 'a + IntoByteSliceMut<'a>,
    T: FromBytes + IntoBytes + KnownLayout + ?Sized,
{
    /// Converts this `Ref` into a mutable reference.
    ///
    /// `into_mut` consumes the `Ref`, and returns a mutable reference to `T`.
    ///
    /// Note: this is an associated function, which means that you have to call
    /// it as `Ref::into_mut(r)` instead of `r.into_mut()`. This is so that
    /// there is no conflict with a method on the inner type.
    #[must_use = "has no side effects"]
    #[inline(always)]
    pub fn into_mut(r: Self) -> &'a mut T {
        static_assert_dst_is_not_zst!(T);

        let b = unsafe { r.into_byte_slice_mut() };

        let ptr = Ptr::from_mut(b.into_byte_slice_mut())
            .try_cast_into_no_leftover::<T, BecauseExclusive>(None)
            .expect("zerocopy internal error: into_ref should be infallible");
        let ptr = ptr.recall_validity::<_, (_, (_, _))>();
        ptr.as_mut()
    }
}

impl<B, T> Ref<B, T>
where
    B: ByteSlice,
    T: ?Sized,
{
    /// Gets the underlying bytes.
    ///
    /// Note: this is an associated function, which means that you have to call
    /// it as `Ref::bytes(r)` instead of `r.bytes()`. This is so that there is
    /// no conflict with a method on the inner type.
    #[inline]
    pub fn bytes(r: &Self) -> &[u8] {
        unsafe { r.as_byte_slice().deref() }
    }
}

impl<B, T> Ref<B, T>
where
    B: ByteSliceMut,
    T: ?Sized,
{
    /// Gets the underlying bytes mutably.
    ///
    /// Note: this is an associated function, which means that you have to call
    /// it as `Ref::bytes_mut(r)` instead of `r.bytes_mut()`. This is so that
    /// there is no conflict with a method on the inner type.
    #[inline]
    pub fn bytes_mut(r: &mut Self) -> &mut [u8] {
        unsafe { r.as_byte_slice_mut().deref_mut() }
    }
}

impl<B, T> Ref<B, T>
where
    B: ByteSlice,
    T: FromBytes,
{
    /// Reads a copy of `T`.
    ///
    /// Note: this is an associated function, which means that you have to call
    /// it as `Ref::read(r)` instead of `r.read()`. This is so that there is no
    /// conflict with a method on the inner type.
    #[must_use = "has no side effects"]
    #[inline]
    pub fn read(r: &Self) -> T {
        let b = unsafe { r.as_byte_slice() };

        unsafe { ptr::read(b.deref().as_ptr().cast::<T>()) }
    }
}

impl<B, T> Ref<B, T>
where
    B: ByteSliceMut,
    T: IntoBytes,
{
    /// Writes the bytes of `t` and then forgets `t`.
    ///
    /// Note: this is an associated function, which means that you have to call
    /// it as `Ref::write(r, t)` instead of `r.write(t)`. This is so that there
    /// is no conflict with a method on the inner type.
    #[inline]
    pub fn write(r: &mut Self, t: T) {
        let b = unsafe { r.as_byte_slice_mut() };

        unsafe { ptr::write(b.deref_mut().as_mut_ptr().cast::<T>(), t) }
    }
}

impl<B, T> Deref for Ref<B, T>
where
    B: ByteSlice,
    T: FromBytes + KnownLayout + Immutable + ?Sized,
{
    type Target = T;
    #[inline]
    fn deref(&self) -> &T {
        static_assert_dst_is_not_zst!(T);

        let b = unsafe { self.as_byte_slice() };

        let ptr = Ptr::from_ref(b.deref())
            .try_cast_into_no_leftover::<T, BecauseImmutable>(None)
            .expect("zerocopy internal error: Deref::deref should be infallible");
        let ptr = ptr.recall_validity();
        ptr.as_ref()
    }
}

impl<B, T> DerefMut for Ref<B, T>
where
    B: ByteSliceMut,
    T: FromBytes + IntoBytes + KnownLayout + Immutable + ?Sized,
{
    #[inline]
    fn deref_mut(&mut self) -> &mut T {
        static_assert_dst_is_not_zst!(T);

        let b = unsafe { self.as_byte_slice_mut() };

        let ptr = Ptr::from_mut(b.deref_mut())
            .try_cast_into_no_leftover::<T, BecauseExclusive>(None)
            .expect("zerocopy internal error: DerefMut::deref_mut should be infallible");
        let ptr = ptr.recall_validity::<_, (_, (_, (BecauseExclusive, BecauseExclusive)))>();
        ptr.as_mut()
    }
}

impl<T, B> Display for Ref<B, T>
where
    B: ByteSlice,
    T: FromBytes + Display + KnownLayout + Immutable + ?Sized,
{
    #[inline]
    fn fmt(&self, fmt: &mut Formatter<'_>) -> fmt::Result {
        let inner: &T = self;
        inner.fmt(fmt)
    }
}

impl<T, B> Debug for Ref<B, T>
where
    B: ByteSlice,
    T: FromBytes + Debug + KnownLayout + Immutable + ?Sized,
{
    #[inline]
    fn fmt(&self, fmt: &mut Formatter<'_>) -> fmt::Result {
        let inner: &T = self;
        fmt.debug_tuple("Ref").field(&inner).finish()
    }
}

impl<T, B> Eq for Ref<B, T>
where
    B: ByteSlice,
    T: FromBytes + Eq + KnownLayout + Immutable + ?Sized,
{
}

impl<T, B> PartialEq for Ref<B, T>
where
    B: ByteSlice,
    T: FromBytes + PartialEq + KnownLayout + Immutable + ?Sized,
{
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        self.deref().eq(other.deref())
    }
}

impl<T, B> Ord for Ref<B, T>
where
    B: ByteSlice,
    T: FromBytes + Ord + KnownLayout + Immutable + ?Sized,
{
    #[inline]
    fn cmp(&self, other: &Self) -> Ordering {
        let inner: &T = self;
        let other_inner: &T = other;
        inner.cmp(other_inner)
    }
}

impl<T, B> PartialOrd for Ref<B, T>
where
    B: ByteSlice,
    T: FromBytes + PartialOrd + KnownLayout + Immutable + ?Sized,
{
    #[inline]
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        let inner: &T = self;
        let other_inner: &T = other;
        inner.partial_cmp(other_inner)
    }
}
