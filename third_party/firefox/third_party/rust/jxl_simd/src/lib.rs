// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

#![allow(clippy::too_many_arguments)]

use std::{
    fmt::Debug,
    mem::MaybeUninit,
    ops::{
        Add, AddAssign, BitAnd, BitAndAssign, BitOr, BitOrAssign, BitXor, BitXorAssign, Div,
        DivAssign, Mul, MulAssign, Neg, Sub, SubAssign,
    },
};

#[cfg(target_arch = "x86_64")]
mod x86_64;

#[cfg(target_arch = "aarch64")]
mod aarch64;

pub mod float16;
pub mod scalar;

pub use float16::f16;

#[cfg(all(target_arch = "x86_64", feature = "avx"))]
pub use x86_64::avx::AvxDescriptor;
#[cfg(all(target_arch = "x86_64", feature = "avx512"))]
pub use x86_64::avx512::Avx512Descriptor;
#[cfg(all(target_arch = "x86_64", feature = "sse42"))]
pub use x86_64::sse42::Sse42Descriptor;

#[cfg(all(target_arch = "aarch64", feature = "neon"))]
pub use aarch64::neon::NeonDescriptor;

pub use scalar::ScalarDescriptor;

pub trait SimdDescriptor: Sized + Copy + Debug + Send + Sync {
    type F32Vec: F32SimdVec<Descriptor = Self>;

    type I32Vec: I32SimdVec<Descriptor = Self>;

    type U32Vec: U32SimdVec<Descriptor = Self>;

    type U16Vec: U16SimdVec<Descriptor = Self>;

    type U8Vec: U8SimdVec<Descriptor = Self>;

    type Mask: SimdMask<Descriptor = Self>;

    /// Prepared 8-entry BF16 lookup table for fast approximate lookups.
    /// Use `F32SimdVec::prepare_table_bf16_8` to create and
    /// `F32SimdVec::table_lookup_bf16_8` to use.
    type Bf16Table8: Copy;

    type Descriptor256: SimdDescriptor<Descriptor256 = Self::Descriptor256>;
    type Descriptor128: SimdDescriptor<Descriptor128 = Self::Descriptor128>;

    fn new() -> Option<Self>;

    /// Returns a vector descriptor suitable for operations on vectors of length 256 (Self if the
    /// current vector type is suitable). Note that it might still be beneficial to use `Self` for
    /// .call(), as the compiler could make use of features from more advanced instruction sets.
    fn maybe_downgrade_256bit(self) -> Self::Descriptor256;

    /// Same as Self::maybe_downgrade_256bit, but for 128 bits.
    fn maybe_downgrade_128bit(self) -> Self::Descriptor128;

    /// Calls the given closure within a target feature context.
    /// This enables establishing an unbroken chain of inline functions from the feature-annotated
    /// gateway up to the closure, allowing SIMD intrinsics to be used safely.
    fn call<R>(self, f: impl FnOnce(Self) -> R) -> R;
}

/// # Safety
///
/// Implementors are required to respect the safety promises of the methods in this trait.
/// Specifically, this applies to the store_*_uninit methods.
pub unsafe trait F32SimdVec:
    Sized
    + Copy
    + Debug
    + Send
    + Sync
    + Add<Self, Output = Self>
    + Mul<Self, Output = Self>
    + Sub<Self, Output = Self>
    + Div<Self, Output = Self>
    + AddAssign<Self>
    + MulAssign<Self>
    + SubAssign<Self>
    + DivAssign<Self>
{
    type Descriptor: SimdDescriptor;

    const LEN: usize;

    /// An array of f32 of length Self::LEN.
    type UnderlyingArray: Copy + Default + Debug;

    /// Converts v to an array of v.
    fn splat(d: Self::Descriptor, v: f32) -> Self;

    fn zero(d: Self::Descriptor) -> Self;

    fn mul_add(self, mul: Self, add: Self) -> Self;

    /// Computes `add - self * mul`, equivalent to `self * (-mul) + add`.
    /// Uses fused multiply-add with negation when available (FMA3 fnmadd).
    fn neg_mul_add(self, mul: Self, add: Self) -> Self;

    fn load(d: Self::Descriptor, mem: &[f32]) -> Self;

    fn load_array(d: Self::Descriptor, mem: &Self::UnderlyingArray) -> Self;

    fn store(&self, mem: &mut [f32]);

    fn store_array(&self, mem: &mut Self::UnderlyingArray);

    /// Stores two vectors interleaved: [a0, b0, a1, b1, a2, b2, ...].
    /// Requires `dest.len() >= 2 * Self::LEN` or it will panic.
    #[inline(always)]
    fn store_interleaved_2(a: Self, b: Self, dest: &mut [f32]) {
        let dest = unsafe {
            std::slice::from_raw_parts_mut(dest.as_mut_ptr().cast::<MaybeUninit<f32>>(), dest.len())
        };
        Self::store_interleaved_2_uninit(a, b, dest);
    }

    /// Stores three vectors interleaved: [a0, b0, c0, a1, b1, c1, ...].
    /// Requires `dest.len() >= 3 * Self::LEN` or it will panic.
    #[inline(always)]
    fn store_interleaved_3(a: Self, b: Self, c: Self, dest: &mut [f32]) {
        let dest = unsafe {
            std::slice::from_raw_parts_mut(dest.as_mut_ptr().cast::<MaybeUninit<f32>>(), dest.len())
        };
        Self::store_interleaved_3_uninit(a, b, c, dest);
    }

    /// Stores four vectors interleaved: [a0, b0, c0, d0, a1, b1, c1, d1, ...].
    /// Requires `dest.len() >= 4 * Self::LEN` or it will panic.
    #[inline(always)]
    fn store_interleaved_4(a: Self, b: Self, c: Self, d: Self, dest: &mut [f32]) {
        let dest = unsafe {
            std::slice::from_raw_parts_mut(dest.as_mut_ptr().cast::<MaybeUninit<f32>>(), dest.len())
        };
        Self::store_interleaved_4_uninit(a, b, c, d, dest);
    }

    /// Stores two vectors interleaved: [a0, b0, a1, b1, a2, b2, ...].
    /// Requires `dest.len() >= 2 * Self::LEN` or it will panic.
    ///
    /// Safety note:
    /// Does not write uninitialized data into `dest`.
    fn store_interleaved_2_uninit(a: Self, b: Self, dest: &mut [MaybeUninit<f32>]);

    /// Stores three vectors interleaved: [a0, b0, c0, a1, b1, c1, ...].
    /// Requires `dest.len() >= 3 * Self::LEN` or it will panic.
    /// Safety note:
    /// Does not write uninitialized data into `dest`.
    fn store_interleaved_3_uninit(a: Self, b: Self, c: Self, dest: &mut [MaybeUninit<f32>]);

    /// Stores four vectors interleaved: [a0, b0, c0, d0, a1, b1, c1, d1, ...].
    /// Requires `dest.len() >= 4 * Self::LEN` or it will panic.
    /// Safety note:
    /// Does not write uninitialized data into `dest`.
    fn store_interleaved_4_uninit(
        a: Self,
        b: Self,
        c: Self,
        d: Self,
        dest: &mut [MaybeUninit<f32>],
    );

    /// Stores eight vectors interleaved: [a0, b0, c0, d0, e0, f0, g0, h0, a1, ...].
    /// Requires `dest.len() >= 8 * Self::LEN` or it will panic.
    fn store_interleaved_8(
        a: Self,
        b: Self,
        c: Self,
        d: Self,
        e: Self,
        f: Self,
        g: Self,
        h: Self,
        dest: &mut [f32],
    );

    /// Loads two vectors from interleaved data: [a0, b0, a1, b1, a2, b2, ...].
    /// Returns (a, b) where a = [a0, a1, a2, ...] and b = [b0, b1, b2, ...].
    /// Requires `src.len() >= 2 * Self::LEN` or it will panic.
    fn load_deinterleaved_2(d: Self::Descriptor, src: &[f32]) -> (Self, Self);

    /// Loads three vectors from interleaved data: [a0, b0, c0, a1, b1, c1, ...].
    /// Returns (a, b, c) where a = [a0, a1, ...], b = [b0, b1, ...], c = [c0, c1, ...].
    /// Requires `src.len() >= 3 * Self::LEN` or it will panic.
    fn load_deinterleaved_3(d: Self::Descriptor, src: &[f32]) -> (Self, Self, Self);

    /// Loads four vectors from interleaved data: [a0, b0, c0, d0, a1, b1, c1, d1, ...].
    /// Returns (a, b, c, d) where each vector contains the deinterleaved components.
    /// Requires `src.len() >= 4 * Self::LEN` or it will panic.
    fn load_deinterleaved_4(d: Self::Descriptor, src: &[f32]) -> (Self, Self, Self, Self);

    /// Rounds to nearest integer and stores as u8.
    /// Behavior is unspecified if values would overflow u8.
    /// Requires `dest.len() >= Self::LEN` or it will panic.
    fn round_store_u8(self, dest: &mut [u8]);

    /// Rounds to nearest integer and stores as u16.
    /// Behavior is unspecified if values would overflow u16.
    /// Requires `dest.len() >= Self::LEN` or it will panic.
    fn round_store_u16(self, dest: &mut [u16]);

    fn abs(self) -> Self;

    fn floor(self) -> Self;

    fn sqrt(self) -> Self;

    /// Negates all elements. Currently unused but kept for API completeness.
    #[allow(dead_code)]
    fn neg(self) -> Self;

    fn copysign(self, sign: Self) -> Self;

    fn max(self, other: Self) -> Self;

    fn min(self, other: Self) -> Self;

    fn gt(self, other: Self) -> <<Self as F32SimdVec>::Descriptor as SimdDescriptor>::Mask;

    fn as_i32(self) -> <<Self as F32SimdVec>::Descriptor as SimdDescriptor>::I32Vec;

    fn bitcast_to_i32(self) -> <<Self as F32SimdVec>::Descriptor as SimdDescriptor>::I32Vec;

    /// Prepares an 8-entry f32 table for fast approximate lookups.
    /// Values are converted to BF16 format (loses lower 16 mantissa bits).
    ///
    /// Use this when you need to perform multiple lookups with the same table.
    /// The prepared table can be reused with [`table_lookup_bf16_8`].
    fn prepare_table_bf16_8(
        d: Self::Descriptor,
        table: &[f32; 8],
    ) -> <<Self as F32SimdVec>::Descriptor as SimdDescriptor>::Bf16Table8;

    /// Performs fast approximate table lookup using a prepared BF16 table.
    ///
    /// This is the fastest lookup method when the same table is used multiple times.
    /// Use [`prepare_table_bf16_8`] to create the prepared table.
    ///
    /// # Panics
    /// May panic or produce undefined results if indices contain values outside 0..8 range.
    fn table_lookup_bf16_8(
        d: Self::Descriptor,
        table: <<Self as F32SimdVec>::Descriptor as SimdDescriptor>::Bf16Table8,
        indices: <<Self as F32SimdVec>::Descriptor as SimdDescriptor>::I32Vec,
    ) -> Self;

    /// Converts a slice of f32 into a slice of Self::UnderlyingArray. If slice.len() is not a
    /// multiple of `Self::LEN` this will panic.
    fn make_array_slice(slice: &[f32]) -> &[Self::UnderlyingArray];

    /// Converts a mut slice of f32 into a slice of Self::UnderlyingArray. If slice.len() is not a
    /// multiple of `Self::LEN` this will panic.
    fn make_array_slice_mut(slice: &mut [f32]) -> &mut [Self::UnderlyingArray];

    /// Transposes the Self::LEN x Self::LEN matrix formed by array elements
    /// `data[stride * i]` for i = 0..Self::LEN.
    fn transpose_square(d: Self::Descriptor, data: &mut [Self::UnderlyingArray], stride: usize);

    /// Loads f16 values (stored as u16 bit patterns) and converts them to f32.
    /// Uses hardware conversion instructions when available (F16C on x86, NEON fp16 on ARM).
    /// Requires `mem.len() >= Self::LEN` or it will panic.
    fn load_f16_bits(d: Self::Descriptor, mem: &[u16]) -> Self;

    /// Converts f32 values to f16 and stores as u16 bit patterns.
    /// Uses hardware conversion instructions when available (F16C on x86, NEON fp16 on ARM).
    /// Requires `dest.len() >= Self::LEN` or it will panic.
    fn store_f16_bits(self, dest: &mut [u16]);
}

pub trait I32SimdVec:
    Sized
    + Copy
    + Debug
    + Send
    + Sync
    + Add<Self, Output = Self>
    + Mul<Self, Output = Self>
    + Sub<Self, Output = Self>
    + Neg<Output = Self>
    + BitAnd<Self, Output = Self>
    + BitOr<Self, Output = Self>
    + BitXor<Self, Output = Self>
    + AddAssign<Self>
    + MulAssign<Self>
    + SubAssign<Self>
    + BitAndAssign<Self>
    + BitOrAssign<Self>
    + BitXorAssign<Self>
{
    type Descriptor: SimdDescriptor;

    #[allow(dead_code)]
    const LEN: usize;

    /// Converts v to an array of v.
    fn splat(d: Self::Descriptor, v: i32) -> Self;

    fn load(d: Self::Descriptor, mem: &[i32]) -> Self;

    fn store(&self, mem: &mut [i32]);

    fn abs(self) -> Self;

    fn as_f32(self) -> <<Self as I32SimdVec>::Descriptor as SimdDescriptor>::F32Vec;

    fn bitcast_to_f32(self) -> <<Self as I32SimdVec>::Descriptor as SimdDescriptor>::F32Vec;

    fn bitcast_to_u32(self) -> <<Self as I32SimdVec>::Descriptor as SimdDescriptor>::U32Vec;

    fn gt(self, other: Self) -> <<Self as I32SimdVec>::Descriptor as SimdDescriptor>::Mask;

    fn lt_zero(self) -> <<Self as I32SimdVec>::Descriptor as SimdDescriptor>::Mask;

    fn eq(self, other: Self) -> <<Self as I32SimdVec>::Descriptor as SimdDescriptor>::Mask;

    fn eq_zero(self) -> <<Self as I32SimdVec>::Descriptor as SimdDescriptor>::Mask;

    fn shl<const AMOUNT_U: u32, const AMOUNT_I: i32>(self) -> Self;

    fn shr<const AMOUNT_U: u32, const AMOUNT_I: i32>(self) -> Self;

    fn mul_wide_take_high(self, rhs: Self) -> Self;

    /// Stores the lower 16 bits of each i32 lane as u16 values.
    /// Requires `dest.len() >= Self::LEN` or it will panic.
    fn store_u16(self, dest: &mut [u16]);

    /// Stores the lower 8 bits of each i32 lane as u8 values.
    /// Requires `dest.len() >= Self::LEN` or it will panic.
    fn store_u8(self, dest: &mut [u8]);
}

pub trait U32SimdVec: Sized + Copy + Debug + Send + Sync {
    type Descriptor: SimdDescriptor;

    #[allow(dead_code)]
    const LEN: usize;

    fn bitcast_to_i32(self) -> <<Self as U32SimdVec>::Descriptor as SimdDescriptor>::I32Vec;

    fn shr<const AMOUNT_U: u32, const AMOUNT_I: i32>(self) -> Self;
}

/// # Safety
///
/// Implementors are required to respect the safety promises of the methods in this trait.
/// Specifically, this applies to the store_*_uninit methods.
pub unsafe trait U8SimdVec: Sized + Copy + Debug + Send + Sync {
    type Descriptor: SimdDescriptor;

    const LEN: usize;

    fn load(d: Self::Descriptor, mem: &[u8]) -> Self;
    fn splat(d: Self::Descriptor, v: u8) -> Self;
    fn store(&self, mem: &mut [u8]);

    /// Stores two vectors interleaved: [a0, b0, a1, b1, a2, b2, ...].
    /// Requires `dest.len() >= 2 * Self::LEN` or it will panic.
    #[inline(always)]
    fn store_interleaved_2(a: Self, b: Self, dest: &mut [u8]) {
        let dest = unsafe {
            std::slice::from_raw_parts_mut(dest.as_mut_ptr().cast::<MaybeUninit<u8>>(), dest.len())
        };
        Self::store_interleaved_2_uninit(a, b, dest);
    }

    /// Stores three vectors interleaved: [a0, b0, c0, a1, b1, c1, ...].
    /// Requires `dest.len() >= 3 * Self::LEN` or it will panic.
    #[inline(always)]
    fn store_interleaved_3(a: Self, b: Self, c: Self, dest: &mut [u8]) {
        let dest = unsafe {
            std::slice::from_raw_parts_mut(dest.as_mut_ptr().cast::<MaybeUninit<u8>>(), dest.len())
        };
        Self::store_interleaved_3_uninit(a, b, c, dest);
    }

    /// Stores four vectors interleaved: [a0, b0, c0, d0, a1, b1, c1, d1, ...].
    /// Requires `dest.len() >= 4 * Self::LEN` or it will panic.
    #[inline(always)]
    fn store_interleaved_4(a: Self, b: Self, c: Self, d: Self, dest: &mut [u8]) {
        let dest = unsafe {
            std::slice::from_raw_parts_mut(dest.as_mut_ptr().cast::<MaybeUninit<u8>>(), dest.len())
        };
        Self::store_interleaved_4_uninit(a, b, c, d, dest);
    }

    /// Stores two vectors interleaved: [a0, b0, a1, b1, a2, b2, ...].
    /// Requires `dest.len() >= 2 * Self::LEN` or it will panic.
    ///
    /// Safety note:
    /// Does not write uninitialized data into `dest`.
    fn store_interleaved_2_uninit(a: Self, b: Self, dest: &mut [MaybeUninit<u8>]);

    /// Stores three vectors interleaved: [a0, b0, c0, a1, b1, c1, ...].
    /// Requires `dest.len() >= 3 * Self::LEN` or it will panic.
    /// Safety note:
    /// Does not write uninitialized data into `dest`.
    fn store_interleaved_3_uninit(a: Self, b: Self, c: Self, dest: &mut [MaybeUninit<u8>]);

    /// Stores four vectors interleaved: [a0, b0, c0, d0, a1, b1, c1, d1, ...].
    /// Requires `dest.len() >= 4 * Self::LEN` or it will panic.
    /// Safety note:
    /// Does not write uninitialized data into `dest`.
    fn store_interleaved_4_uninit(a: Self, b: Self, c: Self, d: Self, dest: &mut [MaybeUninit<u8>]);
}

/// # Safety
///
/// Implementors are required to respect the safety promises of the methods in this trait.
/// Specifically, this applies to the store_*_uninit methods.
pub unsafe trait U16SimdVec: Sized + Copy + Debug + Send + Sync {
    type Descriptor: SimdDescriptor;

    const LEN: usize;

    fn load(d: Self::Descriptor, mem: &[u16]) -> Self;
    fn splat(d: Self::Descriptor, v: u16) -> Self;
    fn store(&self, mem: &mut [u16]);

    /// Stores two vectors interleaved: [a0, b0, a1, b1, a2, b2, ...].
    /// Requires `dest.len() >= 2 * Self::LEN` or it will panic.
    #[inline(always)]
    fn store_interleaved_2(a: Self, b: Self, dest: &mut [u16]) {
        let dest = unsafe {
            std::slice::from_raw_parts_mut(dest.as_mut_ptr().cast::<MaybeUninit<u16>>(), dest.len())
        };
        Self::store_interleaved_2_uninit(a, b, dest);
    }

    /// Stores three vectors interleaved: [a0, b0, c0, a1, b1, c1, ...].
    /// Requires `dest.len() >= 3 * Self::LEN` or it will panic.
    #[inline(always)]
    fn store_interleaved_3(a: Self, b: Self, c: Self, dest: &mut [u16]) {
        let dest = unsafe {
            std::slice::from_raw_parts_mut(dest.as_mut_ptr().cast::<MaybeUninit<u16>>(), dest.len())
        };
        Self::store_interleaved_3_uninit(a, b, c, dest);
    }

    /// Stores four vectors interleaved: [a0, b0, c0, d0, a1, b1, c1, d1, ...].
    /// Requires `dest.len() >= 4 * Self::LEN` or it will panic.
    #[inline(always)]
    fn store_interleaved_4(a: Self, b: Self, c: Self, d: Self, dest: &mut [u16]) {
        let dest = unsafe {
            std::slice::from_raw_parts_mut(dest.as_mut_ptr().cast::<MaybeUninit<u16>>(), dest.len())
        };
        Self::store_interleaved_4_uninit(a, b, c, d, dest);
    }

    /// Stores two vectors interleaved: [a0, b0, a1, b1, a2, b2, ...].
    /// Requires `dest.len() >= 2 * Self::LEN` or it will panic.
    ///
    /// Safety note:
    /// Does not write uninitialized data into `dest`.
    fn store_interleaved_2_uninit(a: Self, b: Self, dest: &mut [MaybeUninit<u16>]);

    /// Stores three vectors interleaved: [a0, b0, c0, a1, b1, c1, ...].
    /// Requires `dest.len() >= 3 * Self::LEN` or it will panic.
    /// Safety note:
    /// Does not write uninitialized data into `dest`.
    fn store_interleaved_3_uninit(a: Self, b: Self, c: Self, dest: &mut [MaybeUninit<u16>]);

    /// Stores four vectors interleaved: [a0, b0, c0, d0, a1, b1, c1, d1, ...].
    /// Requires `dest.len() >= 4 * Self::LEN` or it will panic.
    /// Safety note:
    /// Does not write uninitialized data into `dest`.
    fn store_interleaved_4_uninit(
        a: Self,
        b: Self,
        c: Self,
        d: Self,
        dest: &mut [MaybeUninit<u16>],
    );
}

#[macro_export]
macro_rules! shl {
    ($val: expr, $amount: literal) => {
        $val.shl::<{ $amount as u32 }, { $amount as i32 }>()
    };
}

#[macro_export]
macro_rules! shr {
    ($val: expr, $amount: literal) => {
        $val.shr::<{ $amount as u32 }, { $amount as i32 }>()
    };
}

pub trait SimdMask:
    Sized + Copy + Debug + Send + Sync + BitAnd<Self, Output = Self> + BitOr<Self, Output = Self>
{
    type Descriptor: SimdDescriptor;

    fn if_then_else_f32(
        self,
        if_true: <<Self as SimdMask>::Descriptor as SimdDescriptor>::F32Vec,
        if_false: <<Self as SimdMask>::Descriptor as SimdDescriptor>::F32Vec,
    ) -> <<Self as SimdMask>::Descriptor as SimdDescriptor>::F32Vec;

    fn if_then_else_i32(
        self,
        if_true: <<Self as SimdMask>::Descriptor as SimdDescriptor>::I32Vec,
        if_false: <<Self as SimdMask>::Descriptor as SimdDescriptor>::I32Vec,
    ) -> <<Self as SimdMask>::Descriptor as SimdDescriptor>::I32Vec;

    fn maskz_i32(
        self,
        v: <<Self as SimdMask>::Descriptor as SimdDescriptor>::I32Vec,
    ) -> <<Self as SimdMask>::Descriptor as SimdDescriptor>::I32Vec;

    fn all(self) -> bool;

    fn andnot(self, rhs: Self) -> Self;
}

macro_rules! impl_f32_array_interface {
    () => {
        type UnderlyingArray = [f32; Self::LEN];

        #[inline(always)]
        fn make_array_slice(slice: &[f32]) -> &[Self::UnderlyingArray] {
            let (ret, rem) = slice.as_chunks();
            assert!(rem.is_empty());
            ret
        }

        #[inline(always)]
        fn make_array_slice_mut(slice: &mut [f32]) -> &mut [Self::UnderlyingArray] {
            let (ret, rem) = slice.as_chunks_mut();
            assert!(rem.is_empty());
            ret
        }

        #[inline(always)]
        fn load_array(d: Self::Descriptor, mem: &Self::UnderlyingArray) -> Self {
            Self::load(d, mem)
        }

        #[inline(always)]
        fn store_array(&self, mem: &mut Self::UnderlyingArray) {
            self.store(mem);
        }
    };
}

pub(crate) use impl_f32_array_interface;
