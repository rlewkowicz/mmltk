// Copyright Mozilla Foundation
// Licensed under the Apache License (Version 2.0), or the MIT license,
// (the "Licenses") at your option. You may not use this file except in
// compliance with one of the Licenses. You may obtain copies of the
// Licenses at:
//    https://www.apache.org/licenses/LICENSE-2.0
//    https://opensource.org/licenses/MIT
// Unless required by applicable law or agreed to in writing, software
// distributed under the Licenses is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the Licenses for the specific language governing permissions and
// limitations under the Licenses.

#![no_std]

//! Provides iteration by `char` over `&[u16]` containing potentially-invalid
//! UTF-16 such that errors are replaced with the REPLACEMENT CHARACTER.
//!
//! The trait `Utf16CharsEx` provides the convenience method `chars()` on
//! byte slices themselves instead of having to use the more verbose
//! `Utf16Chars::new(slice)`.

#[cfg(feature = "icu_collections")]
mod cptrie;
#[cfg(feature = "icu_collections")]
mod cptrie_indices;
mod indices;
mod report;

#[cfg(feature = "icu_collections")]
pub use crate::cptrie::Utf16CharsWithTrie;
#[cfg(feature = "icu_collections")]
pub use crate::cptrie::Utf16CharsWithTrieEx;
#[cfg(feature = "icu_collections")]
pub use crate::cptrie_indices::Utf16CharIndicesWithTrie;
pub use crate::indices::Utf16CharIndices;
pub use crate::report::ErrorReportingUtf16Chars;
pub use crate::report::Utf16CharsError;
use core::iter::FusedIterator;

#[inline(always)]
pub(crate) fn in_inclusive_range16(i: u16, start: u16, end: u16) -> bool {
    i.wrapping_sub(start) <= (end - start)
}

/// Iterator by `char` over `&[u16]` that contains
/// potentially-invalid UTF-16. See the crate documentation.
#[derive(Debug, Clone)]
pub struct Utf16Chars<'a> {
    remaining: &'a [u16],
}

impl<'a> Utf16Chars<'a> {
    #[inline(always)]
    /// Creates the iterator from a `u16` slice.
    pub fn new(code_units: &'a [u16]) -> Self {
        Utf16Chars::<'a> {
            remaining: code_units,
        }
    }

    /// Views the current remaining data in the iterator as a subslice
    /// of the original slice.
    #[inline(always)]
    pub fn as_slice(&self) -> &'a [u16] {
        self.remaining
    }

    #[inline(never)]
    fn surrogate_next(&mut self, surrogate_base: u16, first: u16) -> char {
        if surrogate_base <= (0xDBFF - 0xD800) {
            if let Some((&low, tail_tail)) = self.remaining.split_first() {
                if in_inclusive_range16(low, 0xDC00, 0xDFFF) {
                    self.remaining = tail_tail;
                    return unsafe {
                        char::from_u32_unchecked(
                            (u32::from(first) << 10) + u32::from(low)
                                - (((0xD800u32 << 10) - 0x10000u32) + 0xDC00u32),
                        )
                    };
                }
            }
        }
        '\u{FFFD}'
    }

    #[inline(never)]
    fn surrogate_next_back(&mut self, last: u16) -> char {
        if in_inclusive_range16(last, 0xDC00, 0xDFFF) {
            if let Some((&high, head_head)) = self.remaining.split_last() {
                if in_inclusive_range16(high, 0xD800, 0xDBFF) {
                    self.remaining = head_head;
                    return unsafe {
                        char::from_u32_unchecked(
                            (u32::from(high) << 10) + u32::from(last)
                                - (((0xD800u32 << 10) - 0x10000u32) + 0xDC00u32),
                        )
                    };
                }
            }
        }
        '\u{FFFD}'
    }
}

impl<'a> Iterator for Utf16Chars<'a> {
    type Item = char;

    #[inline(always)]
    fn next(&mut self) -> Option<char> {
        let (&first, tail) = self.remaining.split_first()?;
        self.remaining = tail;
        let surrogate_base = first.wrapping_sub(0xD800);
        if surrogate_base > (0xDFFF - 0xD800) {
            return Some(unsafe { char::from_u32_unchecked(u32::from(first)) });
        }
        Some(self.surrogate_next(surrogate_base, first))
    }
}

impl<'a> DoubleEndedIterator for Utf16Chars<'a> {
    #[inline(always)]
    fn next_back(&mut self) -> Option<char> {
        let (&last, head) = self.remaining.split_last()?;
        self.remaining = head;
        if !in_inclusive_range16(last, 0xD800, 0xDFFF) {
            return Some(unsafe { char::from_u32_unchecked(u32::from(last)) });
        }
        Some(self.surrogate_next_back(last))
    }
}

impl FusedIterator for Utf16Chars<'_> {}

/// Convenience trait that adds `chars()` and `char_indices()` methods
/// similar to the ones on string slices to `u16` slices.
pub trait Utf16CharsEx {
    /// Convenience method for creating an UTF-16 iterator
    /// for the slice.
    fn chars(&self) -> Utf16Chars<'_>;

    /// Convenience method for creating a code unit index and
    /// UTF-16 iterator for the slice.
    fn char_indices(&self) -> Utf16CharIndices<'_>;
}

impl Utf16CharsEx for [u16] {
    /// Convenience method for creating an UTF-16 iterator
    /// for the slice.
    #[inline]
    fn chars(&self) -> Utf16Chars<'_> {
        Utf16Chars::new(self)
    }
    /// Convenience method for creating a code unit index and
    /// UTF-16 iterator for the slice.
    #[inline]
    fn char_indices(&self) -> Utf16CharIndices<'_> {
        Utf16CharIndices::new(self)
    }
}
