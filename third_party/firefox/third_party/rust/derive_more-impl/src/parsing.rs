//! Common parsing utilities for derive macros.
//!
//! Fair parsing of [`syn::Expr`] requires [`syn`]'s `full` feature to be enabled, which unnecessary
//! increases compile times. As we don't have complex AST manipulation, usually requiring only
//! understanding where syntax item begins and ends, simpler manual parsing is implemented.

use proc_macro2::{Spacing, TokenStream};
use quote::ToTokens;
use syn::{
    buffer::Cursor,
    parse::{Parse, ParseStream},
};

/// [`syn::Expr`] [`Parse`]ing polyfill.
#[derive(Clone, Debug)]
pub(crate) enum Expr {
    /// [`syn::Expr::Path`] of length 1 [`Parse`]ing polyfill.
    Ident(syn::Ident),

    /// Every other [`syn::Expr`] variant.
    Other(TokenStream),
}

impl Expr {
    /// Returns a [`syn::Ident`] in case this [`Expr`] is represented only by it.
    ///
    /// [`syn::Ident`]: struct@syn::Ident
    pub(crate) fn ident(&self) -> Option<&syn::Ident> {
        match self {
            Self::Ident(ident) => Some(ident),
            Self::Other(_) => None,
        }
    }
}

impl From<syn::Ident> for Expr {
    fn from(ident: syn::Ident) -> Self {
        Self::Ident(ident)
    }
}

impl Parse for Expr {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        if let Ok(ident) = input.step(|c| {
            c.ident()
                .filter(|(_, c)| c.eof() || punct(',')(*c).is_some())
                .ok_or_else(|| syn::Error::new(c.span(), "expected `ident(,|eof)`"))
        }) {
            Ok(Self::Ident(ident))
        } else {
            input.step(|c| {
                take_until1(
                    alt([
                        &mut seq([
                            &mut path_sep,
                            &mut balanced_pair(punct('<'), punct('>')),
                        ]),
                        &mut seq([
                            &mut balanced_pair(punct('<'), punct('>')),
                            &mut path_sep,
                        ]),
                        &mut balanced_pair(punct('|'), punct('|')),
                        &mut token_tree,
                    ]),
                    punct(','),
                )(*c)
                .map(|(stream, cursor)| (Self::Other(stream), cursor))
                .ok_or_else(|| syn::Error::new(c.span(), "failed to parse expression"))
            })
        }
    }
}

impl PartialEq<syn::Ident> for Expr {
    fn eq(&self, other: &syn::Ident) -> bool {
        self.ident().is_some_and(|i| i == other)
    }
}

impl ToTokens for Expr {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        match self {
            Self::Ident(ident) => ident.to_tokens(tokens),
            Self::Other(other) => other.to_tokens(tokens),
        }
    }
}

/// Result of parsing.
type ParsingResult<'a> = Option<(TokenStream, Cursor<'a>)>;

/// Tries to parse a [`token::PathSep`].
///
/// [`token::PathSep`]: struct@syn::token::PathSep
pub fn path_sep(c: Cursor<'_>) -> ParsingResult<'_> {
    seq([
        &mut punct_with_spacing(':', Spacing::Joint),
        &mut punct(':'),
    ])(c)
}

/// Tries to parse a [`punct`] with [`Spacing`].
pub fn punct_with_spacing(
    p: char,
    spacing: Spacing,
) -> impl FnMut(Cursor<'_>) -> ParsingResult<'_> {
    move |c| {
        c.punct().and_then(|(punct, c)| {
            (punct.as_char() == p && punct.spacing() == spacing)
                .then(|| (punct.into_token_stream(), c))
        })
    }
}

/// Tries to parse a [`Punct`].
///
/// [`Punct`]: proc_macro2::Punct
pub fn punct(p: char) -> impl FnMut(Cursor<'_>) -> ParsingResult<'_> {
    move |c| {
        c.punct().and_then(|(punct, c)| {
            (punct.as_char() == p).then(|| (punct.into_token_stream(), c))
        })
    }
}

/// Tries to parse any [`TokenTree`].
///
/// [`TokenTree`]: proc_macro2::TokenTree
pub fn token_tree(c: Cursor<'_>) -> ParsingResult<'_> {
    c.token_tree().map(|(tt, c)| (tt.into_token_stream(), c))
}

/// Parses until balanced amount of `open` and `close` or eof.
///
/// [`Cursor`] should be pointing **right after** the first `open`ing.
pub fn balanced_pair(
    mut open: impl FnMut(Cursor<'_>) -> ParsingResult<'_>,
    mut close: impl FnMut(Cursor<'_>) -> ParsingResult<'_>,
) -> impl FnMut(Cursor<'_>) -> ParsingResult<'_> {
    move |c| {
        let (mut out, mut c) = open(c)?;
        let mut count = 1;

        while count != 0 {
            let (stream, cursor) = if let Some(closing) = close(c) {
                count -= 1;
                closing
            } else if let Some(opening) = open(c) {
                count += 1;
                opening
            } else {
                let (tt, c) = c.token_tree()?;
                (tt.into_token_stream(), c)
            };
            out.extend(stream);
            c = cursor;
        }

        Some((out, c))
    }
}

/// Tries to execute the provided sequence of `parsers`.
pub fn seq<const N: usize>(
    mut parsers: [&mut dyn FnMut(Cursor<'_>) -> ParsingResult<'_>; N],
) -> impl FnMut(Cursor<'_>) -> ParsingResult<'_> + '_ {
    move |c| {
        parsers.iter_mut().try_fold(
            (TokenStream::new(), c),
            |(mut out, mut c), parser| {
                let (stream, cursor) = parser(c)?;
                out.extend(stream);
                c = cursor;
                Some((out, c))
            },
        )
    }
}

/// Tries to execute the first successful parser.
pub fn alt<const N: usize>(
    mut parsers: [&mut dyn FnMut(Cursor<'_>) -> ParsingResult<'_>; N],
) -> impl FnMut(Cursor<'_>) -> ParsingResult<'_> + '_ {
    move |c| parsers.iter_mut().find_map(|parser| parser(c))
}

/// Parses with `basic` while `until` fails. Returns [`None`] in case
/// `until` succeeded initially or `basic` never succeeded. Doesn't consume
/// tokens parsed by `until`.
pub fn take_until1<P, U>(
    mut parser: P,
    mut until: U,
) -> impl FnMut(Cursor<'_>) -> ParsingResult<'_>
where
    P: FnMut(Cursor<'_>) -> ParsingResult<'_>,
    U: FnMut(Cursor<'_>) -> ParsingResult<'_>,
{
    move |mut cursor| {
        let mut out = TokenStream::new();
        let mut parsed = false;

        loop {
            if cursor.eof() || until(cursor).is_some() {
                return parsed.then_some((out, cursor));
            }

            let (stream, c) = parser(cursor)?;
            out.extend(stream);
            cursor = c;
            parsed = true;
        }
    }
}
