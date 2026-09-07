//! Keccak [sponge function](https://en.wikipedia.org/wiki/Sponge_function).
//!
//! If you are looking for SHA-3 hash functions take a look at [`sha3`][1] and
//! [`tiny-keccak`][2] crates.
//!
//! To disable loop unrolling (e.g. for constraint targets) use `no_unroll`
//! feature.
//!
//! ```
//! // Test vectors are from KeccakCodePackage
//! let mut data = [0u64; 25];
//!
//! keccak::f1600(&mut data);
//! assert_eq!(data, [
//!     0xF1258F7940E1DDE7, 0x84D5CCF933C0478A, 0xD598261EA65AA9EE, 0xBD1547306F80494D,
//!     0x8B284E056253D057, 0xFF97A42D7F8E6FD4, 0x90FEE5A0A44647C4, 0x8C5BDA0CD6192E76,
//!     0xAD30A6F71B19059C, 0x30935AB7D08FFC64, 0xEB5AA93F2317D635, 0xA9A6E6260D712103,
//!     0x81A57C16DBCF555F, 0x43B831CD0347C826, 0x01F22F1A11A5569F, 0x05E5635A21D9AE61,
//!     0x64BEFEF28CC970F2, 0x613670957BC46611, 0xB87C5A554FD00ECB, 0x8C3EE88A1CCF32C8,
//!     0x940C7922AE3A2614, 0x1841F924A2C509E4, 0x16F53526E70465C2, 0x75F644E97F30A13B,
//!     0xEAF1FF7B5CECA249,
//! ]);
//!
//! keccak::f1600(&mut data);
//! assert_eq!(data, [
//!     0x2D5C954DF96ECB3C, 0x6A332CD07057B56D, 0x093D8D1270D76B6C, 0x8A20D9B25569D094,
//!     0x4F9C4F99E5E7F156, 0xF957B9A2DA65FB38, 0x85773DAE1275AF0D, 0xFAF4F247C3D810F7,
//!     0x1F1B9EE6F79A8759, 0xE4FECC0FEE98B425, 0x68CE61B6B9CE68A1, 0xDEEA66C4BA8F974F,
//!     0x33C43D836EAFB1F5, 0xE00654042719DBD9, 0x7CF8A9F009831265, 0xFD5449A6BF174743,
//!     0x97DDAD33D8994B40, 0x48EAD5FC5D0BE774, 0xE3B8C8EE55B7B03C, 0x91A0226E649E42E9,
//!     0x900E3129E7BADD7B, 0x202A9EC5FAA3CCE8, 0x5B3402464E1C3DB6, 0x609F4E62A44C1059,
//!     0x20D06CD26A8FBF5C,
//! ]);
//! ```
//!
//! [1]: https://docs.rs/sha3
//! [2]: https://docs.rs/tiny-keccak

#![no_std]
#![cfg_attr(docsrs, feature(doc_auto_cfg))]
#![cfg_attr(feature = "simd", feature(portable_simd))]
#![doc(
    html_logo_url = "https://raw.githubusercontent.com/RustCrypto/meta/master/logo.svg",
    html_favicon_url = "https://raw.githubusercontent.com/RustCrypto/meta/master/logo.svg"
)]
#![allow(non_upper_case_globals)]
#![warn(
    clippy::mod_module_files,
    clippy::unwrap_used,
    missing_docs,
    rust_2018_idioms,
    unused_lifetimes,
    unused_qualifications
)]

use core::{
    convert::TryInto,
    fmt::Debug,
    mem::size_of,
    ops::{BitAnd, BitAndAssign, BitXor, BitXorAssign, Not},
};

#[rustfmt::skip]
mod unroll;

#[cfg(all(target_arch = "aarch64", feature = "asm"))]
mod armv8;

#[cfg(all(target_arch = "aarch64", feature = "asm"))]
cpufeatures::new!(armv8_sha3_intrinsics, "sha3");

const PLEN: usize = 25;

const RHO: [u32; 24] = [
    1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44,
];

const PI: [usize; 24] = [
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1,
];

const RC: [u64; 24] = [
    0x0000000000000001,
    0x0000000000008082,
    0x800000000000808a,
    0x8000000080008000,
    0x000000000000808b,
    0x0000000080000001,
    0x8000000080008081,
    0x8000000000008009,
    0x000000000000008a,
    0x0000000000000088,
    0x0000000080008009,
    0x000000008000000a,
    0x000000008000808b,
    0x800000000000008b,
    0x8000000000008089,
    0x8000000000008003,
    0x8000000000008002,
    0x8000000000000080,
    0x000000000000800a,
    0x800000008000000a,
    0x8000000080008081,
    0x8000000000008080,
    0x0000000080000001,
    0x8000000080008008,
];

/// Keccak is a permutation over an array of lanes which comprise the sponge
/// construction.
pub trait LaneSize:
    Copy
    + Clone
    + Debug
    + Default
    + PartialEq
    + BitAndAssign
    + BitAnd<Output = Self>
    + BitXorAssign
    + BitXor<Output = Self>
    + Not<Output = Self>
{
    /// Number of rounds of the Keccak-f permutation.
    const KECCAK_F_ROUND_COUNT: usize;

    /// Truncate function.
    fn truncate_rc(rc: u64) -> Self;

    /// Rotate left function.
    fn rotate_left(self, n: u32) -> Self;
}

macro_rules! impl_lanesize {
    ($type:ty, $round:expr, $truncate:expr) => {
        impl LaneSize for $type {
            const KECCAK_F_ROUND_COUNT: usize = $round;

            fn truncate_rc(rc: u64) -> Self {
                $truncate(rc)
            }

            fn rotate_left(self, n: u32) -> Self {
                self.rotate_left(n)
            }
        }
    };
}

impl_lanesize!(u8, 18, |rc: u64| { rc.to_le_bytes()[0] });
impl_lanesize!(u16, 20, |rc: u64| {
    let tmp = rc.to_le_bytes();
    #[allow(clippy::unwrap_used)]
    Self::from_le_bytes(tmp[..size_of::<Self>()].try_into().unwrap())
});
impl_lanesize!(u32, 22, |rc: u64| {
    let tmp = rc.to_le_bytes();
    #[allow(clippy::unwrap_used)]
    Self::from_le_bytes(tmp[..size_of::<Self>()].try_into().unwrap())
});
impl_lanesize!(u64, 24, |rc: u64| { rc });

macro_rules! impl_keccak {
    ($pname:ident, $fname:ident, $type:ty) => {
        /// Keccak-p sponge function
        pub fn $pname(state: &mut [$type; PLEN], round_count: usize) {
            keccak_p(state, round_count);
        }

        /// Keccak-f sponge function
        pub fn $fname(state: &mut [$type; PLEN]) {
            keccak_p(state, <$type>::KECCAK_F_ROUND_COUNT);
        }
    };
}

impl_keccak!(p200, f200, u8);
impl_keccak!(p400, f400, u16);
impl_keccak!(p800, f800, u32);

#[cfg(not(all(target_arch = "aarch64", feature = "asm")))]
impl_keccak!(p1600, f1600, u64);

/// Keccak-p[1600, rc] permutation.
#[cfg(all(target_arch = "aarch64", feature = "asm"))]
pub fn p1600(state: &mut [u64; PLEN], round_count: usize) {
    if armv8_sha3_intrinsics::get() {
        unsafe { armv8::p1600_armv8_sha3_asm(state, round_count) }
    } else {
        keccak_p(state, round_count);
    }
}

/// Keccak-f[1600] permutation.
#[cfg(all(target_arch = "aarch64", feature = "asm"))]
pub fn f1600(state: &mut [u64; PLEN]) {
    if armv8_sha3_intrinsics::get() {
        unsafe { armv8::p1600_armv8_sha3_asm(state, 24) }
    } else {
        keccak_p(state, u64::KECCAK_F_ROUND_COUNT);
    }
}

#[cfg(feature = "simd")]
/// SIMD implementations for Keccak-f1600 sponge function
pub mod simd {
    use crate::{keccak_p, LaneSize, PLEN};
    pub use core::simd::{u64x2, u64x4, u64x8};

    macro_rules! impl_lanesize_simd_u64xn {
        ($type:ty) => {
            impl LaneSize for $type {
                const KECCAK_F_ROUND_COUNT: usize = 24;

                fn truncate_rc(rc: u64) -> Self {
                    Self::splat(rc)
                }

                fn rotate_left(self, n: u32) -> Self {
                    self << Self::splat(n.into()) | self >> Self::splat((64 - n).into())
                }
            }
        };
    }

    impl_lanesize_simd_u64xn!(u64x2);
    impl_lanesize_simd_u64xn!(u64x4);
    impl_lanesize_simd_u64xn!(u64x8);

    impl_keccak!(p1600x2, f1600x2, u64x2);
    impl_keccak!(p1600x4, f1600x4, u64x4);
    impl_keccak!(p1600x8, f1600x8, u64x8);
}

#[allow(unused_assignments)]
/// Generic Keccak-p sponge function
pub fn keccak_p<L: LaneSize>(state: &mut [L; PLEN], round_count: usize) {
    if round_count > L::KECCAK_F_ROUND_COUNT {
        panic!("A round_count greater than KECCAK_F_ROUND_COUNT is not supported!");
    }

    let round_consts = &RC[(L::KECCAK_F_ROUND_COUNT - round_count)..L::KECCAK_F_ROUND_COUNT];

    for &rc in round_consts {
        let mut array = [L::default(); 5];

        unroll5!(x, {
            unroll5!(y, {
                array[x] ^= state[5 * y + x];
            });
        });

        unroll5!(x, {
            unroll5!(y, {
                let t1 = array[(x + 4) % 5];
                let t2 = array[(x + 1) % 5].rotate_left(1);
                state[5 * y + x] ^= t1 ^ t2;
            });
        });

        let mut last = state[1];
        unroll24!(x, {
            array[0] = state[PI[x]];
            state[PI[x]] = last.rotate_left(RHO[x]);
            last = array[0];
        });

        unroll5!(y_step, {
            let y = 5 * y_step;

            unroll5!(x, {
                array[x] = state[y + x];
            });

            unroll5!(x, {
                let t1 = !array[(x + 1) % 5];
                let t2 = array[(x + 2) % 5];
                state[y + x] = array[x] ^ (t1 & t2);
            });
        });

        state[0] ^= L::truncate_rc(rc);
    }
}
