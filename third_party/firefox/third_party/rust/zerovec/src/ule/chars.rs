// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

#![allow(clippy::upper_case_acronyms)]
//! ULE implementation for the `char` type.

use super::*;
use crate::impl_ule_from_array;
use core::cmp::Ordering;
use core::convert::TryFrom;

/// A u8 array of little-endian data corresponding to a Unicode scalar value.
///
/// The bytes of a `CharULE` are guaranteed to represent a little-endian-encoded u32 that is a
/// valid `char` and can be converted without validation.
///
/// # Examples
///
/// Convert a `char` to a `CharULE` and back again:
///
/// ```
/// use zerovec::ule::{AsULE, CharULE, ULE};
///
/// let c1 = '𑄃';
/// let ule = c1.to_unaligned();
/// assert_eq!(CharULE::slice_as_bytes(&[ule]), &[0x03, 0x11, 0x01]);
/// let c2 = char::from_unaligned(ule);
/// assert_eq!(c1, c2);
/// ```
///
/// Attempt to parse invalid bytes to a `CharULE`:
///
/// ```
/// use zerovec::ule::{CharULE, ULE};
///
/// let bytes: &[u8] = &[0xFF, 0xFF, 0xFF, 0xFF];
/// CharULE::parse_bytes_to_slice(bytes).expect_err("Invalid bytes");
/// ```
#[repr(transparent)]
#[derive(Debug, PartialEq, Eq, Clone, Copy, Hash)]
pub struct CharULE([u8; 3]);

impl CharULE {
    /// Converts a [`char`] to a [`CharULE`]. This is equivalent to calling
    /// [`AsULE::to_unaligned()`]
    ///
    /// See the type-level documentation for [`CharULE`] for more information.
    #[inline]
    pub const fn from_aligned(c: char) -> Self {
        let [u0, u1, u2, _u3] = (c as u32).to_le_bytes();
        Self([u0, u1, u2])
    }

    /// Converts this [`CharULE`] to a [`char`]. This is equivalent to calling
    /// [`AsULE::from_unaligned`]
    ///
    /// See the type-level documentation for [`CharULE`] for more information.
    #[inline]
    pub fn to_char(self) -> char {
        let [b0, b1, b2] = self.0;
        unsafe { char::from_u32_unchecked(u32::from_le_bytes([b0, b1, b2, 0])) }
    }

    impl_ule_from_array!(char, CharULE, Self([0; 3]));
}

unsafe impl ULE for CharULE {
    #[inline]
    fn validate_bytes(bytes: &[u8]) -> Result<(), UleError> {
        if bytes.len() % 3 != 0 {
            return Err(UleError::length::<Self>(bytes.len()));
        }
        for chunk in bytes.chunks_exact(3) {
            #[expect(clippy::indexing_slicing)]
            let u = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], 0]);
            char::try_from(u).map_err(|_| UleError::parse::<Self>())?;
        }
        Ok(())
    }
}

impl AsULE for char {
    type ULE = CharULE;

    #[inline]
    fn to_unaligned(self) -> Self::ULE {
        CharULE::from_aligned(self)
    }

    #[inline]
    fn from_unaligned(unaligned: Self::ULE) -> Self {
        unaligned.to_char()
    }
}

impl PartialOrd for CharULE {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for CharULE {
    fn cmp(&self, other: &Self) -> Ordering {
        char::from_unaligned(*self).cmp(&char::from_unaligned(*other))
    }
}
