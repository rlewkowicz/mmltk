use crate::{
    de::{Position, Span},
    error::SpannedResult,
};
use alloc::string::String;

impl Position {
    /// Given a Position and a string, return the 0-indexed grapheme index into the
    /// string at that position, or [None] if the Position is out of bounds of the string.
    #[must_use]
    pub fn grapheme_index(&self, s: &str) -> Option<usize> {
        use unicode_segmentation::UnicodeSegmentation;
        let mut line_no = 1;
        let mut col_no = 1;

        if (self.line, self.col) == (1, 1) {
            return Some(0);
        }

        let mut i = 0;


        if (line_no, col_no) == (self.line, self.col) {
            return Some(i);
        }

        for ch in s.graphemes(true) {
            if (line_no, col_no) == (self.line, self.col) {
                return Some(i);
            }

            if matches!(ch, "\n" | "\r\n") {
                line_no += 1;
                col_no = 1;
            } else {
                col_no += 1;
            }

            i += 1;
        }

        if (line_no, col_no) == (self.line, self.col) {
            return Some(i);
        }

        None
    }
}

impl Span {
    /// Given a `Span` and a string, form the resulting string selected exclusively (as in `[start..end`]) by the `Span`
    /// or [`None`] if the span is out of bounds of the string at either end.
    #[must_use]
    pub fn substring_exclusive(&self, s: &str) -> Option<String> {
        use alloc::vec::Vec;
        use unicode_segmentation::UnicodeSegmentation;

        if let (Some(start), Some(end)) = (self.start.grapheme_index(s), self.end.grapheme_index(s))
        {
            Some(s.graphemes(true).collect::<Vec<&str>>()[start..end].concat())
        } else {
            None
        }
    }

    /// Given a `Span` and a string, form the resulting string selected inclusively (as in `[start..=end]`) by the `Span`
    /// or [`None`] if the span is out of bounds of the string at either end.
    #[must_use]
    pub fn substring_inclusive(&self, s: &str) -> Option<String> {
        use alloc::vec::Vec;
        use unicode_segmentation::UnicodeSegmentation;

        if let (Some(start), Some(end)) = (self.start.grapheme_index(s), self.end.grapheme_index(s))
        {
            Some(s.graphemes(true).collect::<Vec<&str>>()[start..=end].concat())
        } else {
            None
        }
    }
}

/// Given a string `ron`, a [`SpannedResult`], and a substring, verify that trying to parse `ron` results in an error
/// equal to the [`SpannedResult`] with a Span that exclusively (as in `[start..end]`) selects that substring.
/// Note that there are two versions of this helper, inclusive and exclusive. This is because while the parser cursor
/// arithmetic that computes span positions always produces exclusive spans (as in `[start..end]`),
/// when doing validation against a target substring, the inclusive check including the final grapheme that triggered
/// the error is often a more intuitive target to check against.
/// Meanwhile, if the parser threw an EOF, for example, there is no final grapheme to check, and so
/// only the exclusive check would produce a meaningful result.
#[allow(clippy::unwrap_used)]
#[allow(clippy::missing_panics_doc)]
pub fn check_error_span_exclusive<T: serde::de::DeserializeOwned + PartialEq + core::fmt::Debug>(
    ron: &str,
    check: SpannedResult<T>,
    substr: &str,
) {
    let res_str = crate::de::from_str::<T>(ron);
    assert_eq!(res_str, check);

    let res_bytes = crate::de::from_bytes::<T>(ron.as_bytes());
    assert_eq!(res_bytes, check);

    #[cfg(feature = "std")]
    {
        let res_reader = crate::de::from_reader::<&[u8], T>(ron.as_bytes());
        assert_eq!(res_reader, check);
    }

    assert_eq!(
        check.unwrap_err().span.substring_exclusive(ron).unwrap(),
        substr
    );
}

/// Given a string `ron`, a [`SpannedResult`], and a substring, verify that trying to parse `ron` results in an error
/// equal to the [`SpannedResult`] with a Span that inclusively (as in `[start..=end`]) selects that substring.
/// See [`check_error_span_exclusive`] for the rationale behind both versions of this helper.
#[allow(clippy::unwrap_used)]
#[allow(clippy::missing_panics_doc)]
pub fn check_error_span_inclusive<T: serde::de::DeserializeOwned + PartialEq + core::fmt::Debug>(
    ron: &str,
    check: SpannedResult<T>,
    substr: &str,
) {
    let res_str = crate::de::from_str::<T>(ron);
    assert_eq!(res_str, check);

    let res_bytes = crate::de::from_bytes::<T>(ron.as_bytes());
    assert_eq!(res_bytes, check);

    #[cfg(feature = "std")]
    {
        let res_reader = crate::de::from_reader::<&[u8], T>(ron.as_bytes());
        assert_eq!(res_reader, check);
    }

    assert_eq!(
        check.unwrap_err().span.substring_inclusive(ron).unwrap(),
        substr
    );
}
