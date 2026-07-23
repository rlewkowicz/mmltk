//! WGSL directives. The focal point of this API is [`DirectiveKind`].
//!
//! See also <https://www.w3.org/TR/WGSL/#directives>.

pub mod enable_extension;
pub(crate) mod language_extension;

use alloc::boxed::Box;

/// A parsed sentinel word indicating the type of directive to be parsed next.
#[derive(Clone, Copy, Debug, Hash, Eq, PartialEq)]
pub(crate) enum DirectiveKind {
    /// A [`crate::diagnostic_filter`].
    Diagnostic,
    /// An [`enable_extension`].
    Enable,
    /// A [`language_extension`].
    Requires,
}

impl DirectiveKind {
    const DIAGNOSTIC: &'static str = "diagnostic";
    const ENABLE: &'static str = "enable";
    const REQUIRES: &'static str = "requires";

    /// Convert from a sentinel word in WGSL into its associated [`DirectiveKind`], if possible.
    pub fn from_ident(s: &str) -> Option<Self> {
        Some(match s {
            Self::DIAGNOSTIC => Self::Diagnostic,
            Self::ENABLE => Self::Enable,
            Self::REQUIRES => Self::Requires,
            _ => return None,
        })
    }
}

impl crate::diagnostic_filter::Severity {
    #[cfg(feature = "wgsl-in")]
    pub(crate) fn report_wgsl_parse_diag<'a>(
        self,
        err: Box<crate::front::wgsl::error::Error<'a>>,
        source: &str,
    ) -> crate::front::wgsl::Result<'a, ()> {
        self.report_diag(err, |e, level| {
            let e = e.as_parse_error(source);
            log::log!(level, "{}", e.emit_to_string(source));
        })
    }
}
