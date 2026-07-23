use super::attr::AttrsHelper;
use proc_macro2::{Span, TokenStream};
use quote::{format_ident, quote};
use syn::{
    punctuated::Punctuated,
    token::{Colon, Comma, PathSep, Plus, Where},
    Data, DataEnum, DataStruct, DeriveInput, Error, Fields, Generics, Ident, Path, PathArguments,
    PathSegment, PredicateType, Result, TraitBound, TraitBoundModifier, Type, TypeParam,
    TypeParamBound, TypePath, WhereClause, WherePredicate,
};

use std::collections::HashMap;

pub(crate) fn derive(input: &DeriveInput) -> Result<TokenStream> {
    let impls = match &input.data {
        Data::Struct(data) => impl_struct(input, data),
        Data::Enum(data) => impl_enum(input, data),
        Data::Union(_) => Err(Error::new_spanned(input, "Unions are not supported")),
    }?;

    let helpers = specialization();
    let dummy_const = format_ident!("_DERIVE_Display_FOR_{}", input.ident);
    Ok(quote! {
        #[allow(non_upper_case_globals, unused_attributes, unused_qualifications)]
        const #dummy_const: () = {
            #helpers
            #impls
        };
    })
}

#[cfg(feature = "std")]
fn specialization() -> TokenStream {
    quote! {
        trait DisplayToDisplayDoc {
            fn __displaydoc_display(&self) -> Self;
        }

        impl<T: core::fmt::Display> DisplayToDisplayDoc for &T {
            fn __displaydoc_display(&self) -> Self {
                self
            }
        }

        extern crate std;

        trait PathToDisplayDoc {
            fn __displaydoc_display(&self) -> std::path::Display<'_>;
        }

        impl PathToDisplayDoc for std::path::Path {
            fn __displaydoc_display(&self) -> std::path::Display<'_> {
                self.display()
            }
        }

        impl PathToDisplayDoc for std::path::PathBuf {
            fn __displaydoc_display(&self) -> std::path::Display<'_> {
                self.display()
            }
        }
    }
}

#[cfg(not(feature = "std"))]
fn specialization() -> TokenStream {
    quote! {}
}

fn impl_struct(input: &DeriveInput, data: &DataStruct) -> Result<TokenStream> {
    let ty = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();
    let where_clause = generate_where_clause(&input.generics, where_clause);

    let helper = AttrsHelper::new(&input.attrs);

    let display = helper.display(&input.attrs)?.map(|display| {
        let pat = match &data.fields {
            Fields::Named(fields) => {
                let var = fields.named.iter().map(|field| &field.ident);
                quote!(Self { #(#var),* })
            }
            Fields::Unnamed(fields) => {
                let var = (0..fields.unnamed.len()).map(|i| format_ident!("_{}", i));
                quote!(Self(#(#var),*))
            }
            Fields::Unit => quote!(_),
        };
        quote! {
            impl #impl_generics core::fmt::Display for #ty #ty_generics #where_clause {
                fn fmt(&self, formatter: &mut core::fmt::Formatter) -> core::fmt::Result {
                    #[allow(unused_variables)]
                    let #pat = self;
                    #display
                }
            }
        }
    });

    Ok(quote! { #display })
}

/// Create a `where` predicate for `ident`, without any [bound][TypeParamBound]s yet.
fn new_empty_where_type_predicate(ident: Ident) -> PredicateType {
    let mut path_segments = Punctuated::<PathSegment, PathSep>::new();
    path_segments.push_value(PathSegment {
        ident,
        arguments: PathArguments::None,
    });
    PredicateType {
        lifetimes: None,
        bounded_ty: Type::Path(TypePath {
            qself: None,
            path: Path {
                leading_colon: None,
                segments: path_segments,
            },
        }),
        colon_token: Colon {
            spans: [Span::call_site()],
        },
        bounds: Punctuated::<TypeParamBound, Plus>::new(),
    }
}

/// Create a `where` clause that we can add [WherePredicate]s to.
fn new_empty_where_clause() -> WhereClause {
    WhereClause {
        where_token: Where {
            span: Span::call_site(),
        },
        predicates: Punctuated::<WherePredicate, Comma>::new(),
    }
}

enum UseGlobalPrefix {
    LeadingColon,
    #[allow(dead_code)]
    NoLeadingColon,
}

/// Create a path with segments composed of [Idents] *without* any [PathArguments].
fn join_paths(name_segments: &[&str], use_global_prefix: UseGlobalPrefix) -> Path {
    let mut segments = Punctuated::<PathSegment, PathSep>::new();
    assert!(!name_segments.is_empty());
    segments.push_value(PathSegment {
        ident: Ident::new(name_segments[0], Span::call_site()),
        arguments: PathArguments::None,
    });
    for name in name_segments[1..].iter() {
        segments.push_punct(PathSep {
            spans: [Span::call_site(), Span::mixed_site()],
        });
        segments.push_value(PathSegment {
            ident: Ident::new(name, Span::call_site()),
            arguments: PathArguments::None,
        });
    }
    Path {
        leading_colon: match use_global_prefix {
            UseGlobalPrefix::LeadingColon => Some(PathSep {
                spans: [Span::call_site(), Span::mixed_site()],
            }),
            UseGlobalPrefix::NoLeadingColon => None,
        },
        segments,
    }
}

/// Push `new_type_predicate` onto the end of `where_clause`.
fn append_where_clause_type_predicate(
    where_clause: &mut WhereClause,
    new_type_predicate: PredicateType,
) {
    if !where_clause.predicates.is_empty() {
        where_clause.predicates.push_punct(Comma {
            spans: [Span::call_site()],
        });
    }
    where_clause
        .predicates
        .push_value(WherePredicate::Type(new_type_predicate));
}

/// Add a requirement for [core::fmt::Display] to a `where` predicate for some type.
fn add_display_constraint_to_type_predicate(
    predicate_that_needs_a_display_impl: &mut PredicateType,
) {
    let display_path = join_paths(&["core", "fmt", "Display"], UseGlobalPrefix::LeadingColon);

    let display_bound = TypeParamBound::Trait(TraitBound {
        paren_token: None,
        modifier: TraitBoundModifier::None,
        lifetimes: None,
        path: display_path,
    });
    if !predicate_that_needs_a_display_impl.bounds.is_empty() {
        predicate_that_needs_a_display_impl.bounds.push_punct(Plus {
            spans: [Span::call_site()],
        });
    }

    predicate_that_needs_a_display_impl
        .bounds
        .push_value(display_bound);
}

/// Map each declared generic type parameter to the set of all trait boundaries declared on it.
///
/// These boundaries may come from the declaration site:
///     pub enum E<T: MyTrait> { ... }
/// or a `where` clause after the parameter declarations:
///     pub enum E<T> where T: MyTrait { ... }
/// This method will return the boundaries from both of those cases.
fn extract_trait_constraints_from_source(
    where_clause: &WhereClause,
    type_params: &[&TypeParam],
) -> HashMap<Ident, Vec<TraitBound>> {
    let mut param_constraint_mapping: HashMap<Ident, Vec<TraitBound>> = type_params
        .iter()
        .map(|type_param| {
            let trait_bounds: Vec<TraitBound> = type_param
                .bounds
                .iter()
                .flat_map(|bound| match bound {
                    TypeParamBound::Trait(trait_bound) => Some(trait_bound),
                    _ => None,
                })
                .cloned()
                .collect();
            (type_param.ident.clone(), trait_bounds)
        })
        .collect();

    for predicate in where_clause.predicates.iter() {
        if let WherePredicate::Type(ref pred_ty) = predicate {
            let ident = match &pred_ty.bounded_ty {
                Type::Path(TypePath { path, qself: None }) => match path.get_ident() {
                    None => continue,
                    Some(ident) => ident,
                },
                _ => continue,
            };
            if let Some((_, ref mut known_bounds)) = param_constraint_mapping
                .iter_mut()
                .find(|(id, _)| *id == ident)
            {
                for bound in pred_ty.bounds.iter() {
                    if let TypeParamBound::Trait(ref bound) = bound {
                        known_bounds.push(bound.clone());
                    }
                }
            }
        }
    }

    param_constraint_mapping
}

/// Hygienically add `where _: Display` to the set of [TypeParamBound]s for `ident`, creating such
/// a set if necessary.
fn ensure_display_in_where_clause_for_type(where_clause: &mut WhereClause, ident: Ident) {
    for pred_ty in where_clause
        .predicates
        .iter_mut()
        .flat_map(|predicate| match predicate {
            WherePredicate::Type(pred_ty) => Some(pred_ty),
            _ => None,
        })
    {
        let matches_desired_type = matches!(
            &pred_ty.bounded_ty,
            Type::Path(TypePath { path, .. }) if Some(&ident) == path.get_ident());
        if matches_desired_type {
            add_display_constraint_to_type_predicate(pred_ty);
            return;
        }
    }

    let mut new_type_predicate = new_empty_where_type_predicate(ident);
    add_display_constraint_to_type_predicate(&mut new_type_predicate);
    append_where_clause_type_predicate(where_clause, new_type_predicate);
}

/// For all declared type parameters, add a [core::fmt::Display] constraint, unless the type
/// parameter already has any type constraint.
fn ensure_where_clause_has_display_for_all_unconstrained_members(
    where_clause: &mut WhereClause,
    type_params: &[&TypeParam],
) {
    let param_constraint_mapping = extract_trait_constraints_from_source(where_clause, type_params);

    for (ident, known_bounds) in param_constraint_mapping.into_iter() {
        if known_bounds.is_empty() {
            ensure_display_in_where_clause_for_type(where_clause, ident);
        }
    }
}

/// Generate a `where` clause that ensures all generic type parameters `impl`
/// [core::fmt::Display] unless already constrained.
///
/// This approach allows struct/enum definitions deriving [crate::Display] to avoid hardcoding
/// a [core::fmt::Display] constraint into every type parameter.
///
/// If the type parameter isn't already constrained, we add a `where _: Display` clause to our
/// display implementation to expect to be able to format every enum case or struct member.
///
/// In fact, we would preferably only require `where _: Display` or `where _: Debug` where the
/// format string actually requires it. However, while [`std::fmt` defines a formal syntax for
/// `format!()`][format syntax], it *doesn't* expose the actual logic to parse the format string,
/// which appears to live in [`rustc_parse_format`]. While we use the [`syn`] crate to parse rust
/// syntax, it also doesn't currently provide any method to introspect a `format!()` string. It
/// would be nice to contribute this upstream in [`syn`].
///
/// [format syntax]: std::fmt#syntax
/// [`rustc_parse_format`]: https://doc.rust-lang.org/nightly/nightly-rustc/rustc_parse_format/index.html
fn generate_where_clause(generics: &Generics, where_clause: Option<&WhereClause>) -> WhereClause {
    let mut where_clause = where_clause.cloned().unwrap_or_else(new_empty_where_clause);
    let type_params: Vec<&TypeParam> = generics.type_params().collect();
    ensure_where_clause_has_display_for_all_unconstrained_members(&mut where_clause, &type_params);
    where_clause
}

fn impl_enum(input: &DeriveInput, data: &DataEnum) -> Result<TokenStream> {
    let ty = &input.ident;
    let (impl_generics, ty_generics, where_clause) = input.generics.split_for_impl();
    let where_clause = generate_where_clause(&input.generics, where_clause);

    let helper = AttrsHelper::new(&input.attrs);

    let displays = data
        .variants
        .iter()
        .map(|variant| helper.display_with_input(&input.attrs, &variant.attrs))
        .collect::<Result<Vec<_>>>()?;

    if data.variants.is_empty() {
        Ok(quote! {
            impl #impl_generics core::fmt::Display for #ty #ty_generics #where_clause {
                fn fmt(&self, formatter: &mut core::fmt::Formatter) -> core::fmt::Result {
                    unreachable!("empty enums cannot be instantiated and thus cannot be printed")
                }
            }
        })
    } else if displays.iter().any(Option::is_some) {
        let arms = data
            .variants
            .iter()
            .zip(displays)
            .map(|(variant, display)| {
                let display =
                    display.ok_or_else(|| Error::new_spanned(variant, "missing doc comment"))?;
                let ident = &variant.ident;
                Ok(match &variant.fields {
                    Fields::Named(fields) => {
                        let var = fields.named.iter().map(|field| &field.ident);
                        quote!(Self::#ident { #(#var),* } => { #display })
                    }
                    Fields::Unnamed(fields) => {
                        let var = (0..fields.unnamed.len()).map(|i| format_ident!("_{}", i));
                        quote!(Self::#ident(#(#var),*) => { #display })
                    }
                    Fields::Unit => quote!(Self::#ident => { #display }),
                })
            })
            .collect::<Result<Vec<_>>>()?;
        Ok(quote! {
            impl #impl_generics core::fmt::Display for #ty #ty_generics #where_clause {
                fn fmt(&self, formatter: &mut core::fmt::Formatter) -> core::fmt::Result {
                    #[allow(unused_variables)]
                    match self {
                        #(#arms,)*
                    }
                }
            }
        })
    } else {
        Err(Error::new_spanned(input, "Missing doc comments"))
    }
}
