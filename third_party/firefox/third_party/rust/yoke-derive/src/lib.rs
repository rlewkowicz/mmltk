// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

//! Custom derives for `Yokeable` from the `yoke` crate.

use proc_macro::TokenStream;
use proc_macro2::{Span, TokenStream as TokenStream2};
use quote::quote;
use syn::spanned::Spanned;
use syn::{parse_macro_input, parse_quote, DeriveInput, Ident, Lifetime, Type, WherePredicate};
use synstructure::Structure;

mod visitor;

/// Custom derive for `yoke::Yokeable`,
///
/// If your struct contains `zerovec::ZeroMap`, then the compiler will not
/// be able to guarantee the lifetime covariance due to the generic types on
/// the `ZeroMap` itself. You must add the following attribute in order for
/// the custom derive to work with `ZeroMap`.
///
/// ```rust,ignore
/// #[derive(Yokeable)]
/// #[yoke(prove_covariance_manually)]
/// ```
///
/// Beyond this case, if the derive fails to compile due to lifetime issues, it
/// means that the lifetime is not covariant and `Yokeable` is not safe to implement.
#[proc_macro_derive(Yokeable, attributes(yoke))]
pub fn yokeable_derive(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    TokenStream::from(yokeable_derive_impl(&input))
}

fn yokeable_derive_impl(input: &DeriveInput) -> TokenStream2 {
    let tybounds = input
        .generics
        .type_params()
        .map(|ty| {
            let mut ty = ty.clone();
            ty.eq_token = None;
            ty.default = None;
            ty
        })
        .collect::<Vec<_>>();
    let typarams = tybounds
        .iter()
        .map(|ty| ty.ident.clone())
        .collect::<Vec<_>>();
    let static_bounds: Vec<WherePredicate> = typarams
        .iter()
        .map(|ty| parse_quote!(#ty: 'static))
        .collect();
    let lts = input.generics.lifetimes().count();
    if lts == 0 {
        let name = &input.ident;
        quote! {
            unsafe impl<'a, #(#tybounds),*> yoke::Yokeable<'a> for #name<#(#typarams),*> where #(#static_bounds,)* Self: Sized {
                type Output = Self;
                #[inline]
                fn transform(&self) -> &Self::Output {
                    self
                }
                #[inline]
                fn transform_owned(self) -> Self::Output {
                    self
                }
                #[inline]
                unsafe fn make(this: Self::Output) -> Self {
                    this
                }
                #[inline]
                fn transform_mut<F>(&'a mut self, f: F)
                where
                    F: 'static + for<'b> FnOnce(&'b mut Self::Output) {
                    f(self)
                }
            }
        }
    } else {
        if lts != 1 {
            return syn::Error::new(
                input.generics.span(),
                "derive(Yokeable) cannot have multiple lifetime parameters",
            )
            .to_compile_error();
        }
        let name = &input.ident;
        let manual_covariance = input.attrs.iter().any(|a| {
            if let Ok(i) = a.parse_args::<Ident>() {
                if i == "prove_covariance_manually" {
                    return true;
                }
            }
            false
        });
        if manual_covariance {
            let mut structure = Structure::new(input);
            let generics_env = typarams.iter().cloned().collect();
            let static_bounds: Vec<WherePredicate> = typarams
                .iter()
                .map(|ty| parse_quote!(#ty: 'static))
                .collect();
            let mut yoke_bounds: Vec<WherePredicate> = vec![];
            structure.bind_with(|_| synstructure::BindStyle::Move);
            let owned_body = structure.each_variant(|vi| {
                vi.construct(|f, i| {
                    let binding = format!("__binding_{i}");
                    let field = Ident::new(&binding, Span::call_site());
                    let fty_static = replace_lifetime(&f.ty, static_lt());

                    let (has_ty, has_lt) = visitor::check_type_for_parameters(&f.ty, &generics_env);
                    if has_ty {
                        if has_lt {
                            let fty_a = replace_lifetime(&f.ty, custom_lt("'a"));
                            yoke_bounds.push(
                                parse_quote!(#fty_static: yoke::Yokeable<'a, Output = #fty_a>),
                            );
                        } else {
                            yoke_bounds.push(
                                parse_quote!(#fty_static: yoke::Yokeable<'a, Output = #fty_static>),
                            );
                        }
                    }
                    if has_ty || has_lt {
                        quote! {
                            <#fty_static as yoke::Yokeable<'a>>::transform_owned(#field)
                        }
                    } else {
                        quote! { #field }
                    }
                })
            });
            let borrowed_body = structure.each(|binding| {
                let f = binding.ast();
                let field = &binding.binding;

                let (has_ty, has_lt) = visitor::check_type_for_parameters(&f.ty, &generics_env);

                if has_ty || has_lt {
                    let fty_static = replace_lifetime(&f.ty, static_lt());
                    let fty_a = replace_lifetime(&f.ty, custom_lt("'a"));
                    quote! {
                        let _: &#fty_a = &<#fty_static as yoke::Yokeable<'a>>::transform(#field);
                    }
                } else {
                    quote! {}
                }
            });
            return quote! {
                unsafe impl<'a, #(#tybounds),*> yoke::Yokeable<'a> for #name<'static, #(#typarams),*>
                    where #(#static_bounds,)*
                    #(#yoke_bounds,)* {
                    type Output = #name<'a, #(#typarams),*>;
                    #[inline]
                    fn transform(&'a self) -> &'a Self::Output {
                        if false {
                            match self {
                                #borrowed_body
                            }
                        }
                        unsafe {
                            ::core::mem::transmute(self)
                        }
                    }
                    #[inline]
                    fn transform_owned(self) -> Self::Output {
                        match self { #owned_body }
                    }
                    #[inline]
                    unsafe fn make(this: Self::Output) -> Self {
                        use core::{mem, ptr};
                        debug_assert!(mem::size_of::<Self::Output>() == mem::size_of::<Self>());
                        let ptr: *const Self = (&this as *const Self::Output).cast();
                        #[allow(forgetting_copy_types, clippy::forget_copy, clippy::forget_non_drop)] 
                        mem::forget(this);
                        ptr::read(ptr)
                    }
                    #[inline]
                    fn transform_mut<F>(&'a mut self, f: F)
                    where
                        F: 'static + for<'b> FnOnce(&'b mut Self::Output) {
                        unsafe { f(core::mem::transmute::<&'a mut Self, &'a mut Self::Output>(self)) }
                    }
                }
            };
        }
        quote! {
            unsafe impl<'a, #(#tybounds),*> yoke::Yokeable<'a> for #name<'static, #(#typarams),*> where #(#static_bounds,)* {
                type Output = #name<'a, #(#typarams),*>;
                #[inline]
                fn transform(&'a self) -> &'a Self::Output {
                    self
                }
                #[inline]
                fn transform_owned(self) -> Self::Output {
                    self
                }
                #[inline]
                unsafe fn make(this: Self::Output) -> Self {
                    use core::{mem, ptr};
                    debug_assert!(mem::size_of::<Self::Output>() == mem::size_of::<Self>());
                    let ptr: *const Self = (&this as *const Self::Output).cast();
                    #[allow(forgetting_copy_types, clippy::forget_copy, clippy::forget_non_drop)] 
                    mem::forget(this);
                    ptr::read(ptr)
                }
                #[inline]
                fn transform_mut<F>(&'a mut self, f: F)
                where
                    F: 'static + for<'b> FnOnce(&'b mut Self::Output) {
                    unsafe { f(core::mem::transmute::<&'a mut Self, &'a mut Self::Output>(self)) }
                }
            }
        }
    }
}

fn static_lt() -> Lifetime {
    Lifetime::new("'static", Span::call_site())
}

fn custom_lt(s: &str) -> Lifetime {
    Lifetime::new(s, Span::call_site())
}

fn replace_lifetime(x: &Type, lt: Lifetime) -> Type {
    use syn::fold::Fold;
    struct ReplaceLifetime(Lifetime);

    impl Fold for ReplaceLifetime {
        fn fold_lifetime(&mut self, _: Lifetime) -> Lifetime {
            self.0.clone()
        }
    }
    ReplaceLifetime(lt).fold_type(x.clone())
}
