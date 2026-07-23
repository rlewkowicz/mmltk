//! The futures-rs procedural macro implementations.

#![doc(test(
    no_crate_inject,
    attr(
        deny(warnings, rust_2018_idioms, single_use_lifetimes),
        allow(dead_code, unused_assignments, unused_variables)
    )
))]

use proc_macro::TokenStream;

mod join;
mod select;
mod stream_select;

/// The `join!` macro.
#[proc_macro]
pub fn join_internal(input: TokenStream) -> TokenStream {
    crate::join::join(input)
}

/// The `try_join!` macro.
#[proc_macro]
pub fn try_join_internal(input: TokenStream) -> TokenStream {
    crate::join::try_join(input)
}

/// The `select!` macro.
#[proc_macro]
pub fn select_internal(input: TokenStream) -> TokenStream {
    crate::select::select(input)
}

/// The `select_biased!` macro.
#[proc_macro]
pub fn select_biased_internal(input: TokenStream) -> TokenStream {
    crate::select::select_biased(input)
}

/// The `stream_select!` macro.
#[proc_macro]
pub fn stream_select_internal(input: TokenStream) -> TokenStream {
    crate::stream_select::stream_select(input.into())
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}
