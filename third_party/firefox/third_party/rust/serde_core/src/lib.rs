//! Serde is a framework for ***ser***ializing and ***de***serializing Rust data
//! structures efficiently and generically.
//!
//! The `serde_core` crate contains Serde's trait definitions with **no support
//! for #\[derive()\]**.
//!
//! In crates that derive an implementation of `Serialize` or `Deserialize`, you
//! must depend on the [`serde`] crate, not `serde_core`.
//!
//! [`serde`]: https://crates.io/crates/serde
//!
//! In crates that handwrite implementations of Serde traits, or only use them
//! as trait bounds, depending on `serde_core` is permitted. But `serde`
//! re-exports all of these traits and can be used for this use case too. If in
//! doubt, disregard `serde_core` and always use `serde`.
//!
//! Crates that depend on `serde_core` instead of `serde` are able to compile in
//! parallel with `serde_derive` even when `serde`'s "derive" feature is turned on,
//! as shown in the following build timings.
//!
//! <br>
//!
//! <table>
//! <tr><td align="center">When <code>serde_json</code> depends on <code>serde</code></td></tr>
//! <tr><td><img src="https://github.com/user-attachments/assets/78dc179c-6ab1-4059-928c-1474b0d9d0bb"></td></tr>
//! </table>
//!
//! <br>
//!
//! <table>
//! <tr><td align="center">When <code>serde_json</code> depends on <code>serde_core</code></td></tr>
//! <tr><td><img src="https://github.com/user-attachments/assets/6b6cff5e-3e45-4ac7-9db1-d99ee8b9f5f7"></td></tr>
//! </table>

////////////////////////////////////////////////////////////////////////////////

#![doc(html_root_url = "https://docs.rs/serde_core/1.0.228")]
#![cfg_attr(not(feature = "std"), no_std)]
#![cfg_attr(docsrs, feature(doc_cfg, rustdoc_internals))]
#![cfg_attr(docsrs, allow(internal_features))]
#![cfg_attr(feature = "unstable", feature(never_type))]
#![allow(unknown_lints, bare_trait_objects, deprecated)]
#![allow(
    clippy::unnested_or_patterns,
    clippy::semicolon_if_nothing_returned,
    clippy::empty_enum,
    clippy::type_repetition_in_bounds, 
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    clippy::cast_lossless,
    clippy::module_name_repetitions,
    clippy::single_match_else,
    clippy::type_complexity,
    clippy::use_self,
    clippy::zero_prefixed_literal,
    clippy::derive_partial_eq_without_eq,
    clippy::enum_glob_use,
    clippy::explicit_auto_deref,
    clippy::incompatible_msrv,
    clippy::let_underscore_untyped,
    clippy::map_err_ignore,
    clippy::new_without_default,
    clippy::result_unit_err,
    clippy::wildcard_imports,
    clippy::needless_pass_by_value,
    clippy::similar_names,
    clippy::too_many_lines,
    clippy::doc_markdown,
    clippy::elidable_lifetime_names,
    clippy::needless_lifetimes,
    clippy::unseparated_literal_suffix,
    clippy::needless_doctest_main,
    clippy::missing_errors_doc,
    clippy::must_use_candidate,
)]
#![deny(clippy::question_mark_used)]
#![deny(missing_docs, unused_imports)]

////////////////////////////////////////////////////////////////////////////////

#[cfg(feature = "alloc")]
extern crate alloc;

#[macro_use]
mod crate_root;
#[macro_use]
mod macros;

crate_root!();

#[macro_export]
#[doc(hidden)]
macro_rules! __require_serde_not_serde_core {
    () => {
        ::core::compile_error!(
            "Serde derive requires a dependency on the serde crate, not serde_core"
        );
    };
}
