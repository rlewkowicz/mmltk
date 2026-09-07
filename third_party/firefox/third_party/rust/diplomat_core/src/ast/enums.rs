use serde::Serialize;

use super::docs::Docs;
use super::{AttrInheritContext, Attrs, Ident, Method};
use quote::ToTokens;

/// A fieldless enum declaration in an FFI module.
#[derive(Clone, Serialize, Debug, Hash, PartialEq, Eq)]
#[non_exhaustive]
pub struct Enum {
    pub name: Ident,
    pub docs: Docs,
    /// A list of variants of the enum. (name, discriminant, docs, attrs)
    pub variants: Vec<(Ident, isize, Docs, Attrs)>,
    pub methods: Vec<Method>,
    pub attrs: Attrs,
}

impl Enum {
    /// Extract an [`Enum`] metadata value from an AST node.
    pub fn new(enm: &syn::ItemEnum, parent_attrs: &Attrs) -> Enum {
        let mut last_discriminant = -1;
        if !enm.generics.params.is_empty() {
            panic!("Enums cannot have generic parameters");
        }

        let mut attrs = parent_attrs.clone();
        attrs.add_attrs(&enm.attrs);
        let variant_parent_attrs = attrs.attrs_for_inheritance(AttrInheritContext::Variant);

        Enum {
            name: (&enm.ident).into(),
            docs: Docs::from_attrs(&enm.attrs),
            variants: enm
                .variants
                .iter()
                .map(|v| {
                    if !matches!(v.fields, syn::Fields::Unit) {
                        panic!("Enums cannot have fields, we only support C-like enums");
                    }
                    let new_discriminant = v
                        .discriminant
                        .as_ref()
                        .map(|d| {
                            let lit: Result<syn::Lit, _> = syn::parse2(d.1.to_token_stream());
                            if let Ok(syn::Lit::Int(ref lit_int)) = lit {
                                lit_int.base10_parse::<isize>().unwrap()
                            } else {
                                panic!("Expected a discriminant to be a constant integer");
                            }
                        })
                        .unwrap_or_else(|| last_discriminant + 1);

                    last_discriminant = new_discriminant;
                    let mut v_attrs = variant_parent_attrs.clone();
                    v_attrs.add_attrs(&v.attrs);
                    (
                        (&v.ident).into(),
                        new_discriminant,
                        Docs::from_attrs(&v.attrs),
                        v_attrs,
                    )
                })
                .collect(),
            methods: vec![],
            attrs,
        }
    }
}
