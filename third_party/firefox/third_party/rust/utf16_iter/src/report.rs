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

use crate::in_inclusive_range16;
use core::fmt::Formatter;
use core::iter::FusedIterator;

/// A type for signaling UTF-16 errors.
///
/// The value of the unpaired surrogate is not exposed in order
/// to keep the `Result` type (and `Option`-wrapping thereof)
/// the same size as `char`. See an [issue about the representation][1].
///
/// Note: `core::error::Error` is not implemented due to implementing it
/// being an [unstable feature][2] at the time of writing.
///
/// [1]: https://github.com/rust-lang/rust/issues/118367
/// [2]: https://github.com/rust-lang/rust/issues/103765
#[derive(Debug, PartialEq)]
#[non_exhaustive]
pub struct Utf16CharsError;

impl core::fmt::Display for Utf16CharsError {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), core::fmt::Error> {
        write!(f, "unpaired surrogate")
    }
}

/// Iterator by `Result<char,Utf16CharsError>` over `&[u16]` that contains
/// potentially-invalid UTF-16. There is exactly one `Utf16CharsError` per
/// each unpaired surrogate.
#[derive(Debug, Clone)]
pub struct ErrorReportingUtf16Chars<'a> {
    remaining: &'a [u16],
}

impl<'a> ErrorReportingUtf16Chars<'a> {
    #[inline(always)]
    /// Creates the iterator from a `u16` slice.
    pub fn new(code_units: &'a [u16]) -> Self {
        ErrorReportingUtf16Chars::<'a> {
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
    fn surrogate_next(&mut self, surrogate_base: u16, first: u16) -> Result<char, Utf16CharsError> {
        if surrogate_base <= (0xDBFF - 0xD800) {
            if let Some((&low, tail_tail)) = self.remaining.split_first() {
                if in_inclusive_range16(low, 0xDC00, 0xDFFF) {
                    self.remaining = tail_tail;
                    return Ok(unsafe {
                        char::from_u32_unchecked(
                            (u32::from(first) << 10) + u32::from(low)
                                - (((0xD800u32 << 10) - 0x10000u32) + 0xDC00u32),
                        )
                    });
                }
            }
        }
        Err(Utf16CharsError)
    }

    #[inline(never)]
    fn surrogate_next_back(&mut self, last: u16) -> Result<char, Utf16CharsError> {
        if in_inclusive_range16(last, 0xDC00, 0xDFFF) {
            if let Some((&high, head_head)) = self.remaining.split_last() {
                if in_inclusive_range16(high, 0xD800, 0xDBFF) {
                    self.remaining = head_head;
                    return Ok(unsafe {
                        char::from_u32_unchecked(
                            (u32::from(high) << 10) + u32::from(last)
                                - (((0xD800u32 << 10) - 0x10000u32) + 0xDC00u32),
                        )
                    });
                }
            }
        }
        Err(Utf16CharsError)
    }
}

impl<'a> Iterator for ErrorReportingUtf16Chars<'a> {
    type Item = Result<char, Utf16CharsError>;

    #[inline(always)]
    fn next(&mut self) -> Option<Result<char, Utf16CharsError>> {
        let (&first, tail) = self.remaining.split_first()?;
        self.remaining = tail;
        let surrogate_base = first.wrapping_sub(0xD800);
        if surrogate_base > (0xDFFF - 0xD800) {
            return Some(Ok(unsafe { char::from_u32_unchecked(u32::from(first)) }));
        }
        Some(self.surrogate_next(surrogate_base, first))
    }
}

impl<'a> DoubleEndedIterator for ErrorReportingUtf16Chars<'a> {
    #[inline(always)]
    fn next_back(&mut self) -> Option<Result<char, Utf16CharsError>> {
        let (&last, head) = self.remaining.split_last()?;
        self.remaining = head;
        if !in_inclusive_range16(last, 0xD800, 0xDFFF) {
            return Some(Ok(unsafe { char::from_u32_unchecked(u32::from(last)) }));
        }
        Some(self.surrogate_next_back(last))
    }
}

impl FusedIterator for ErrorReportingUtf16Chars<'_> {}
