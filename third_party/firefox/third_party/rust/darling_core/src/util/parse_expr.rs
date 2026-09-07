//! Functions to use with `#[darling(with = "...")]` that control how quoted values
//! in [`Meta`] instances are parsed into [`Expr`] fields.
//!
//! Version 1 of syn did not permit expressions on the right-hand side of the `=` in a
//! [`MetaNameValue`](syn::MetaNameValue), so darling accepted string literals and then
//! parsed their contents as expressions.
//! Passing a string literal in this version would have required the use of a raw string
//! to add quotation marks inside the literal.
//!
//! Version 2 of syn removes the requirement that the right-hand side be a literal.
//! For most types, such as [`Path`](syn::Path), the [`FromMeta`] impl can accept the
//! version without quotation marks without causing ambiguity; a path cannot start and
//! end with quotation marks, so removal is automatic.
//!
//! [`Expr`] is the one type where this ambiguity is new and unavoidable. To address this,
//! this module provides different functions for different expected behaviors.

use syn::{Expr, Meta};

use crate::{Error, FromMeta};

/// Parse a [`Meta`] to an [`Expr`]; if the value is a string literal, the emitted
/// expression will be a string literal.
pub fn preserve_str_literal(meta: &Meta) -> crate::Result<Expr> {
    match meta {
        Meta::Path(_) => Err(Error::unsupported_format("path").with_span(meta)),
        Meta::List(_) => Err(Error::unsupported_format("list").with_span(meta)),
        Meta::NameValue(nv) => Ok(nv.value.clone()),
    }
}

/// Parse a [`Meta`] to an [`Expr`]; if the value is a string literal, the string's
/// contents will be parsed as an expression and emitted.
pub fn parse_str_literal(meta: &Meta) -> crate::Result<Expr> {
    match meta {
        Meta::Path(_) => Err(Error::unsupported_format("path").with_span(meta)),
        Meta::List(_) => Err(Error::unsupported_format("list").with_span(meta)),
        Meta::NameValue(nv) => {
            if let Expr::Lit(expr_lit) = &nv.value {
                Expr::from_value(&expr_lit.lit)
            } else {
                Ok(nv.value.clone())
            }
        }
    }
}
