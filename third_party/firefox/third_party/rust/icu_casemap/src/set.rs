// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use icu_collections::codepointinvlist::CodePointInversionListBuilder;

/// An object that accepts characters and/or strings
/// to be used with [`CaseMapCloserBorrowed::add_string_case_closure_to()`]
/// and [`CaseMapCloserBorrowed::add_case_closure_to()`].
///
/// Usually this object
/// will be some kind of set over codepoints and strings, or something that
/// can be built into one.
///
/// An implementation is provided for [`CodePointInversionListBuilder`], but users are encouraged
/// to implement this trait on their own collections as needed.
///
/// [`CaseMapCloserBorrowed::add_string_case_closure_to()`]: crate::CaseMapCloserBorrowed::add_string_case_closure_to
/// [`CaseMapCloserBorrowed::add_case_closure_to()`]: crate::CaseMapCloserBorrowed::add_case_closure_to
pub trait ClosureSink {
    /// Add a character to the set
    fn add_char(&mut self, c: char);
    /// Add a string to the set
    fn add_string(&mut self, string: &str);
}

impl ClosureSink for CodePointInversionListBuilder {
    fn add_char(&mut self, c: char) {
        self.add_char(c)
    }

    #[inline]
    fn add_string(&mut self, _string: &str) {}
}
