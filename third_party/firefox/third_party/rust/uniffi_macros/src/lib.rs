/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#![cfg_attr(feature = "nightly", feature(proc_macro_expand))]
#![warn(rust_2018_idioms, unused_qualifications)]
#![allow(dead_code)]

//! Macros for `uniffi`.

#[cfg(feature = "trybuild")]
use camino::Utf8Path;
use proc_macro::TokenStream;
use quote::quote;
use syn::{parse_macro_input, LitStr};

mod custom;
mod default;
mod derive;
mod enum_;
mod error;
mod export;
mod ffiops;
mod fnsig;
mod object;
mod record;
mod remote;
mod setup_scaffolding;
mod util;

use self::{
    derive::DeriveOptions, enum_::expand_enum, error::expand_error, export::expand_export,
    object::expand_object, record::expand_record,
};

/// Top-level initialization macro
///
/// The optional namespace argument is only used by the scaffolding templates to pass in the
/// CI namespace.
#[proc_macro]
pub fn setup_scaffolding(tokens: TokenStream) -> TokenStream {
    let namespace = match syn::parse_macro_input!(tokens as Option<LitStr>) {
        Some(lit_str) => lit_str.value(),
        None => match util::mod_path() {
            Ok(v) => v,
            Err(e) => return e.into_compile_error().into(),
        },
    };
    setup_scaffolding::setup_scaffolding(namespace)
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

#[proc_macro_attribute]
pub fn export(attr_args: TokenStream, input: TokenStream) -> TokenStream {
    do_export(attr_args, input, true, false)
}

fn do_export(
    attr_args: TokenStream,
    input: TokenStream,
    keep_input: bool,
    udl_mode: bool,
) -> TokenStream {
    let gen_output = || {
        let item = syn::parse(input)?;
        let altered_input = keep_input.then(|| export::alter_input(&item));
        let output = expand_export(item, attr_args, udl_mode)?;
        Ok(quote! {
            #altered_input
            #output
        })
    };
    gen_output()
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

#[doc(hidden)]
#[proc_macro_attribute]
pub fn export_for_udl(attrs: TokenStream, input: TokenStream) -> TokenStream {
    do_export(attrs, input, false, true)
}

#[doc(hidden)]
#[proc_macro_attribute]
pub fn export_for_udl_derive(attrs: TokenStream, input: TokenStream) -> TokenStream {
    do_export(attrs, input, true, true)
}

#[proc_macro_derive(Record, attributes(uniffi))]
pub fn derive_record(input: TokenStream) -> TokenStream {
    expand_record(parse_macro_input!(input), DeriveOptions::default())
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

#[proc_macro_derive(Enum, attributes(uniffi))]
pub fn derive_enum(input: TokenStream) -> TokenStream {
    expand_enum(parse_macro_input!(input), DeriveOptions::default())
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

#[proc_macro_derive(Object, attributes(uniffi))]
pub fn derive_object(input: TokenStream) -> TokenStream {
    expand_object(parse_macro_input!(input), DeriveOptions::default())
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

#[proc_macro_derive(Error, attributes(uniffi))]
pub fn derive_error(input: TokenStream) -> TokenStream {
    expand_error(parse_macro_input!(input), DeriveOptions::default())
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

/// Generate FFI code for a custom type
#[proc_macro]
pub fn custom_type(tokens: TokenStream) -> TokenStream {
    custom::expand_custom_type(parse_macro_input!(tokens))
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

/// Generate FFI code for a custom newtype
#[proc_macro]
pub fn custom_newtype(tokens: TokenStream) -> TokenStream {
    custom::expand_custom_newtype(parse_macro_input!(tokens))
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

/// `#[remote(<kind>)]` attribute
///
/// `remote()` generates the same code that `#[derive(uniffi::<kind>)]` would, except it only
/// implements the FFI traits for the local UniFfiTag.
///
/// Use this to wrap the definition of an item defined in a remote crate.
/// See `<https://mozilla.github.io/uniffi-rs/udl/remote_ext_types.html>` for details.
#[doc(hidden)]
#[proc_macro_attribute]
pub fn remote(attrs: TokenStream, input: TokenStream) -> TokenStream {
    derive::expand_derive(
        parse_macro_input!(attrs),
        parse_macro_input!(input),
        DeriveOptions::remote(),
    )
    .unwrap_or_else(syn::Error::into_compile_error)
    .into()
}

/// `#[udl_remote(<kind>)]` attribute
///
/// Alternate version of `#[remote]` for UDL-based generation
///
/// The difference is that it doesn't generate metadata items, since we get those from parsing the
/// UDL.
#[doc(hidden)]
#[proc_macro_attribute]
pub fn udl_remote(attrs: TokenStream, input: TokenStream) -> TokenStream {
    derive::expand_derive(
        parse_macro_input!(attrs),
        parse_macro_input!(input),
        DeriveOptions::udl_remote(),
    )
    .unwrap_or_else(syn::Error::into_compile_error)
    .into()
}

/// Derive items for UDL mode
///
/// The Askama templates generate placeholder items wrapped with the `#[udl_derive(<kind>)]`
/// attribute.  The macro code then generates derived items based on the input.  This system ensures
/// that the same code path is used for UDL-based code and proc-macros.
///
/// `udl_derive` works almost exactly like the `derive_*` macros, except it doesn't generate
/// metadata items, since we get those from parsing the UDL.
#[doc(hidden)]
#[proc_macro_attribute]
pub fn udl_derive(attrs: TokenStream, input: TokenStream) -> TokenStream {
    derive::expand_derive(
        parse_macro_input!(attrs),
        parse_macro_input!(input),
        DeriveOptions::udl_derive(),
    )
    .unwrap_or_else(syn::Error::into_compile_error)
    .into()
}

/// A helper macro to include generated component scaffolding.
///
/// This is a simple convenience macro to include the UniFFI component
/// scaffolding as built by `uniffi_build::generate_scaffolding`.
/// Use it like so:
///
/// ```rs
/// uniffi_macros::include_scaffolding!("my_component_name");
/// ```
///
/// This will expand to the appropriate `include!` invocation to include
/// the generated `my_component_name.uniffi.rs` (which it assumes has
/// been successfully built by your crate's `build.rs` script).
#[proc_macro]
pub fn include_scaffolding(udl_stem: TokenStream) -> TokenStream {
    let udl_stem = syn::parse_macro_input!(udl_stem as LitStr);
    if std::env::var("OUT_DIR").is_err() {
        quote! {
            compile_error!("This macro assumes the crate has a build.rs script, but $OUT_DIR is not present");
        }
    } else {
        let toml_path = match util::manifest_path() {
            Ok(path) => path.display().to_string(),
            Err(_) => {
                return quote! {
                    compile_error!("This macro assumes the crate has a build.rs script, but $OUT_DIR is not present");
                }.into();
            }
        };

        quote! {
            #[allow(dead_code)]
            mod __unused {
                const _: &[u8] = include_bytes!(#toml_path);
            }

            include!(concat!(env!("OUT_DIR"), "/", #udl_stem, ".uniffi.rs"));
        }
    }.into()
}

/// Use the FFI trait implementations defined in another crate for a remote type
///
/// See `<https://mozilla.github.io/uniffi-rs/udl/remote_ext_types.html>` for details.
#[proc_macro]
pub fn use_remote_type(tokens: TokenStream) -> TokenStream {
    remote::expand_remote_type(parse_macro_input!(tokens)).into()
}

/// A helper macro to generate and include component scaffolding.
///
/// This is a convenience macro designed for writing `trybuild`-style tests and
/// probably shouldn't be used for production code. Given the path to a `.udl` file,
/// if will run `uniffi-bindgen` to produce the corresponding Rust scaffolding and then
/// include it directly into the calling file. Like so:
///
/// ```rs
/// uniffi_macros::generate_and_include_scaffolding!("path/to/my/interface.udl");
/// ```
#[proc_macro]
#[cfg(feature = "trybuild")]
pub fn generate_and_include_scaffolding(udl_file: TokenStream) -> TokenStream {
    let udl_file = syn::parse_macro_input!(udl_file as LitStr);
    let udl_file_string = udl_file.value();
    let udl_file_path = Utf8Path::new(&udl_file_string);
    if std::env::var("OUT_DIR").is_err() {
        quote! {
            compile_error!("This macro assumes the crate has a build.rs script, but $OUT_DIR is not present");
        }
    } else if let Err(e) = uniffi_build::generate_scaffolding(udl_file_path) {
        let err = format!("{e:#}");
        quote! {
            compile_error!(concat!("Failed to generate scaffolding from UDL file at ", #udl_file, ": ", #err));
        }
    } else {
        let name = LitStr::new(udl_file_path.file_stem().unwrap(), udl_file.span());
        quote! {
            uniffi_macros::include_scaffolding!(#name);
        }
    }.into()
}

/// An attribute for constructors.
///
/// Constructors are in `impl` blocks which have a `#[uniffi::export]` attribute,
///
/// This exists so `#[uniffi::export]` can emit its input verbatim without
/// causing unexpected errors in the entire exported block.
/// This happens very often when the proc-macro is run on an incomplete
/// input by rust-analyzer while the developer is typing.
///
/// So much better to do nothing here then let the impl block find the attribute.
#[proc_macro_attribute]
pub fn constructor(_attrs: TokenStream, input: TokenStream) -> TokenStream {
    input
}

/// An attribute for methods.
///
/// Everything above applies here too.
#[proc_macro_attribute]
pub fn method(_attrs: TokenStream, input: TokenStream) -> TokenStream {
    input
}

/// Attribute for trait interfaces defined in UDL
#[proc_macro_attribute]
pub fn trait_interface(_attr_args: TokenStream, input: TokenStream) -> TokenStream {
    export::alter_trait(&parse_macro_input!(input)).into()
}
