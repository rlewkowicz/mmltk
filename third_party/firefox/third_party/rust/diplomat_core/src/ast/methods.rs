use serde::{Deserialize, Serialize};
use std::ops::ControlFlow;

use super::docs::Docs;
use super::{Attrs, Ident, Lifetime, LifetimeEnv, Mutability, PathType, TypeName};

/// A method declared in the `impl` associated with an FFI struct.
/// Includes both static and non-static methods, which can be distinguished
/// by inspecting [`Method::self_param`].
#[derive(Clone, PartialEq, Eq, Hash, Serialize, Debug)]
#[non_exhaustive]
pub struct Method {
    /// The name of the method as initially declared.
    pub name: Ident,

    /// Lines of documentation for the method.
    pub docs: Docs,

    /// The name of the generated `extern "C"` function
    pub abi_name: Ident,

    /// The `self` param of the method, if any.
    pub self_param: Option<SelfParam>,

    pub self_type: Option<PathType>,

    /// All non-`self` params taken by the method.
    pub params: Vec<Param>,

    /// The return type of the method, if any.
    pub return_type: Option<TypeName>,

    /// The lifetimes introduced in this method and surrounding impl block.
    pub lifetime_env: LifetimeEnv,

    /// The list of `cfg` attributes (if any).
    ///
    /// These are strings instead of `syn::Attribute` or `proc_macro2::TokenStream`
    /// because those types are not `PartialEq`, `Hash`, `Serialize`, etc.
    pub attrs: Attrs,
}

impl Method {
    /// Extracts a [`Method`] from an AST node inside an `impl`.
    pub fn from_syn(
        m: &syn::ImplItemFn,
        self_path_type: PathType,
        impl_generics: Option<&syn::Generics>,
        impl_attrs: &Attrs,
    ) -> Method {
        let mut attrs = impl_attrs.clone();
        attrs.add_attrs(&m.attrs);

        let self_ident = self_path_type.path.elements.last().unwrap();
        let method_ident = &m.sig.ident;
        let concat_method_ident = format!("{self_ident}_{method_ident}");
        let extern_ident = syn::Ident::new(
            &attrs.abi_rename.apply(concat_method_ident.into()),
            m.sig.ident.span(),
        );

        let all_params = m
            .sig
            .inputs
            .iter()
            .filter_map(|a| match a {
                syn::FnArg::Receiver(_) => None,
                syn::FnArg::Typed(ref t) => Some(Param::from_syn(t, self_path_type.clone())),
            })
            .collect::<Vec<_>>();

        let self_param = m
            .sig
            .receiver()
            .map(|rec| SelfParam::from_syn(rec, self_path_type.clone()));

        let return_ty = match &m.sig.output {
            syn::ReturnType::Type(_, return_typ) => {
                Some(TypeName::from_syn(
                    return_typ.as_ref(),
                    Some(self_path_type.clone()),
                ))
            }
            syn::ReturnType::Default => None,
        };

        let lifetime_env = LifetimeEnv::from_method_item(
            m,
            impl_generics,
            self_param.as_ref(),
            &all_params[..],
            return_ty.as_ref(),
        );

        Method {
            name: Ident::from(method_ident),
            docs: Docs::from_attrs(&m.attrs),
            abi_name: Ident::from(&extern_ident),
            self_param,
            self_type: Some(self_path_type),
            params: all_params,
            return_type: return_ty,
            lifetime_env,
            attrs,
        }
    }

    /// Returns the parameters that the output is lifetime-bound to.
    ///
    /// # Examples
    ///
    /// Given the following method:
    /// ```ignore
    /// fn foo<'a, 'b: 'a, 'c>(&'a self, bar: Bar<'b>, baz: Baz<'c>) -> FooBar<'a> { ... }
    /// ```
    /// Then this method would return the `&'a self` and `bar: Bar<'b>` params
    /// because `'a` is in the return type, and `'b` must live at least as long
    /// as `'a`. It wouldn't include `baz: Baz<'c>` though, because the return
    /// type isn't bound by `'c` in any way.
    ///
    /// # Panics
    ///
    /// This method may panic if `TypeName::check_result_type_validity` (called by
    /// `Method::check_validity`) doesn't pass first, since the result type may
    /// contain elided lifetimes that we depend on for this method. The validity
    /// checks ensure that the return type doesn't elide any lifetimes, ensuring
    /// that this method will produce correct results.
    pub fn borrowed_params(&self) -> BorrowedParams<'_> {
        if let Some(ref return_type) = self.return_type {
            let lifetimes = return_type.longer_lifetimes(&self.lifetime_env);

            let held_self_param = self.self_param.as_ref().filter(|self_param| {
                if let Some((Lifetime::Named(ref name), _)) = self_param.reference {
                    if lifetimes.contains(&name) {
                        return true;
                    }
                }
                self_param.path_type.lifetimes.iter().any(|lt| {
                    if let Lifetime::Named(name) = lt {
                        lifetimes.contains(&name)
                    } else {
                        false
                    }
                })
            });

            let held_params = self
                .params
                .iter()
                .filter_map(|param| {
                    let mut lt_kind = LifetimeKind::ReturnValue;
                    param
                        .ty
                        .visit_lifetimes(&mut |lt, _| {
                            match lt {
                                Lifetime::Named(name) if lifetimes.contains(&name) => {
                                    return ControlFlow::Break(());
                                }
                                Lifetime::Static => {
                                    lt_kind = LifetimeKind::Static;
                                    return ControlFlow::Break(());
                                }
                                _ => {}
                            };
                            ControlFlow::Continue(())
                        })
                        .is_break()
                        .then_some((param, lt_kind))
                })
                .collect();

            BorrowedParams(held_self_param, held_params)
        } else {
            BorrowedParams(None, vec![])
        }
    }

    /// Checks whether the method qualifies for special write handling.
    /// To qualify, a method must:
    ///  - not return any value
    ///  - have the last argument be an `&mut diplomat_runtime::DiplomatWrite`
    ///
    /// Typically, methods of this form will be transformed in the bindings to a
    /// method that doesn't take the write as an argument but instead creates
    /// one locally and just returns the final string.
    pub fn is_write_out(&self) -> bool {
        let return_compatible = self
            .return_type
            .as_ref()
            .map(|return_type| match return_type {
                TypeName::Unit => true,
                TypeName::Result(ok, _, _) | TypeName::Option(ok, _) => {
                    matches!(ok.as_ref(), TypeName::Unit)
                }

                _ => false,
            })
            .unwrap_or(true);

        return_compatible && self.params.last().map(Param::is_write).unwrap_or(false)
    }

    /// Checks if any parameters are write (regardless of other compatibilities for write output)
    pub fn has_write_param(&self) -> bool {
        self.params.iter().any(|p| p.is_write())
    }

    /// Returns the documentation block
    pub fn docs(&self) -> &Docs {
        &self.docs
    }
}

/// The `self` parameter taken by a [`Method`].
#[derive(Clone, PartialEq, Eq, Hash, Serialize, Debug)]
#[non_exhaustive]
pub struct SelfParam {
    /// The lifetime and mutability of the `self` param, if it's a reference.
    pub reference: Option<(Lifetime, Mutability)>,

    /// The type of the parameter, which will be a named reference to
    /// the associated struct,
    pub path_type: PathType,

    /// Associated attributes with this self parameter. Used in Demo Generation, mostly.
    pub attrs: Attrs,
}

impl SelfParam {
    pub fn to_typename(&self) -> TypeName {
        let typ = TypeName::Named(self.path_type.clone());
        if let Some((ref lifetime, ref mutability)) = self.reference {
            return TypeName::Reference(lifetime.clone(), *mutability, Box::new(typ));
        }
        typ
    }

    pub fn from_syn(rec: &syn::Receiver, path_type: PathType) -> Self {
        SelfParam {
            reference: rec
                .reference
                .as_ref()
                .map(|(_, lt)| (lt.into(), Mutability::from_syn(&rec.mutability))),
            path_type,
            attrs: Attrs::from_attrs(&rec.attrs),
        }
    }
}

/// The `self` parameter taken by a [`TraitMethod`].
#[derive(Clone, PartialEq, Eq, Hash, Serialize, Debug, Deserialize)]
#[non_exhaustive]
pub struct TraitSelfParam {
    /// The lifetime and mutability of the `self` param, if it's a reference.
    pub reference: Option<(Lifetime, Mutability)>,

    /// The trait of the parameter, which will be a named reference to
    /// the associated trait,
    pub path_trait: PathType,
}

impl TraitSelfParam {
    pub fn to_typename(&self) -> TypeName {
        let typ = TypeName::ImplTrait(self.path_trait.clone());
        if let Some((ref lifetime, ref mutability)) = self.reference {
            return TypeName::Reference(lifetime.clone(), *mutability, Box::new(typ));
        }
        typ
    }

    pub fn from_syn(rec: &syn::Receiver, path_trait: PathType) -> Self {
        TraitSelfParam {
            reference: rec
                .reference
                .as_ref()
                .map(|(_, lt)| (lt.into(), Mutability::from_syn(&rec.mutability))),
            path_trait,
        }
    }
}

/// A parameter taken by a [`Method`], not including `self`.
#[derive(Clone, PartialEq, Eq, Hash, Serialize, Debug)]
#[non_exhaustive]
pub struct Param {
    /// The name of the parameter in the original method declaration.
    pub name: Ident,

    /// The type of the parameter.
    pub ty: TypeName,

    /// Parameter attributes (like #[diplomat::demo(label = "Out")])
    pub attrs: Attrs,
}

impl Param {
    /// Check if this parameter is a Write
    pub fn is_write(&self) -> bool {
        match self.ty {
            TypeName::Reference(_, Mutability::Mutable, ref w) => **w == TypeName::Write,
            _ => false,
        }
    }

    pub fn from_syn(t: &syn::PatType, self_path_type: PathType) -> Self {
        let ident = match t.pat.as_ref() {
            syn::Pat::Ident(ident) => ident,
            _ => panic!("Unexpected param type"),
        };

        let attrs = Attrs::from_attrs(&t.attrs);

        Param {
            name: (&ident.ident).into(),
            ty: TypeName::from_syn(&t.ty, Some(self_path_type)),
            attrs,
        }
    }
}

/// The type of lifetime.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum LifetimeKind {
    /// Param must live at least as long as the returned object.
    ReturnValue,
    /// Param must live for the duration of the program.
    Static,
}

#[derive(Default, Debug)]
/// Parameters in a method that might be borrowed in the return type.
#[non_exhaustive]
pub struct BorrowedParams<'a>(
    pub Option<&'a SelfParam>,
    pub Vec<(&'a Param, LifetimeKind)>,
);

impl BorrowedParams<'_> {
    /// Returns an [`Iterator`] through the names of the parameters that are borrowed
    /// for the lifetime of the return value, accepting an `Ident` that the `self`
    /// param will be called if present.
    pub fn return_names<'a>(&'a self, self_name: &'a Ident) -> impl Iterator<Item = &'a Ident> {
        self.0.iter().map(move |_| self_name).chain(
            self.1
                .iter()
                .filter(|(_, ltk)| *ltk == LifetimeKind::ReturnValue)
                .map(|(param, _)| &param.name),
        )
    }

    /// Returns an [`Iterator`] through the names of the parameters that are borrowed for a
    /// static lifetime.
    pub fn static_names(&self) -> impl Iterator<Item = &'_ Ident> {
        self.1
            .iter()
            .filter(|(_, ltk)| *ltk == LifetimeKind::Static)
            .map(|(param, _)| &param.name)
    }

    /// Returns `true` if a provided param name is included in the borrowed params,
    /// otherwise `false`.
    ///
    /// This method doesn't check the `self` parameter. Use
    /// [`BorrowedParams::borrows_self`] instead.
    pub fn contains(&self, param_name: &Ident) -> bool {
        self.1.iter().any(|(param, _)| &param.name == param_name)
    }

    /// Returns `true` if there are no borrowed parameters, otherwise `false`.
    pub fn is_empty(&self) -> bool {
        self.0.is_none() && self.1.is_empty()
    }

    /// Returns `true` if the `self` param is borrowed, otherwise `false`.
    pub fn borrows_self(&self) -> bool {
        self.0.is_some()
    }

    /// Returns `true` if there are any borrowed params, otherwise `false`.
    pub fn borrows_params(&self) -> bool {
        !self.1.is_empty()
    }

    /// Returns the number of borrowed params.
    pub fn len(&self) -> usize {
        self.1.len() + usize::from(self.0.is_some())
    }
}
