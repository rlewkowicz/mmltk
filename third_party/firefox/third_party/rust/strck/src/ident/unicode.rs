//! Checked strings containing Unicode identifiers according to the
//! [Unicode Standard Annex #31](https://www.unicode.org/reports/tr31/).
//!
//! # Examples
//!
//! ```rust
//! use strck::{IntoCk, ident::unicode::UnicodeIdent};
//!
//! assert!("foo".ck::<UnicodeIdent>().is_ok());
//! assert!("struct".ck::<UnicodeIdent>().is_ok());
//! assert!("Москва".ck::<UnicodeIdent>().is_ok());
//! assert!("東京".ck::<UnicodeIdent>().is_ok());
//!
//! assert!("_identifier".ck::<UnicodeIdent>().is_err());
//! assert!("r#try".ck::<UnicodeIdent>().is_err());
//! assert!("👍".ck::<UnicodeIdent>().is_err());
//! ```
//!
//! # Aliases
//!
//! This module exposes [`Ident`] and [`IdentBuf`], which alias `Ck<UnicodeIdent>`
//! and `Check<UnicodeIdent>` respectively. These aliases are preferred to keep
//! type signatures succinct.
//!
//! These are also exported under the root, and can be accessed as
//! `strck_ident::Ident` and `strck_ident::IdentBuf`.
use crate::{Check, Ck, Invariant};
use core::fmt;

/// An [`Invariant`] for unicode identifiers according to
/// [Unicode Standard Annex #31](https://www.unicode.org/reports/tr31/).
///
/// # Invariants
///
/// * The string is nonempty.
/// * The first character is XID_Start.
/// * Any following characters are XID_Continue.
#[derive(Clone, Debug)]
pub struct UnicodeIdent;

/// Borrowed checked string containing a Unicode identifier.
///
/// See [`UnicodeIdent`] for more details.
pub type Ident = Ck<UnicodeIdent>;

/// Owned checked string containing a Unicode identifier.
///
/// See [`UnicodeIdent`] for more details.
pub type IdentBuf<B = String> = Check<UnicodeIdent, B>;

/// The error type returned from checking invariants of [`UnicodeIdent`].
#[derive(Debug, Copy, Clone, Hash, PartialEq, Eq, PartialOrd, Ord)]
pub enum Error {
    /// Empty string.
    Empty,

    /// The first character isn't XID_Start.
    Start(char),

    /// A trailing character isn't XID_Continue.
    Continue(char),
}

impl std::error::Error for Error {}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Error::Empty => f.pad("empty"),
            Error::Start(ch) => write!(f, "invalid start '{ch}'"),
            Error::Continue(ch) => write!(f, "invalid continue '{ch}'"),
        }
    }
}

impl Invariant for UnicodeIdent {
    type Error = Error;

    fn check(slice: &str) -> Result<(), Self::Error> {
        let mut chars = slice.chars();
        let start = chars.next().ok_or(Error::Empty)?;

        if !unicode_ident::is_xid_start(start) {
            return Err(Error::Start(start));
        }

        for ch in chars {
            if !unicode_ident::is_xid_continue(ch) {
                return Err(Error::Continue(ch));
            }
        }
        Ok(())
    }
}
