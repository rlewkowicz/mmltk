/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! # Enum definitions for a `ComponentInterface`.
//!
//! This module converts enum definition from UDL into structures that can be
//! added to a `ComponentInterface`. A declaration in the UDL like this:
//!
//! ```
//! # let ci = uniffi_bindgen::interface::ComponentInterface::from_webidl(r##"
//! # namespace example {};
//! enum Example {
//!   "one",
//!   "two"
//! };
//! # "##, "crate_name")?;
//! # Ok::<(), anyhow::Error>(())
//! ```
//!
//! Will result in a [`Enum`] member being added to the resulting [`crate::ComponentInterface`]:
//!
//! ```
//! # let ci = uniffi_bindgen::interface::ComponentInterface::from_webidl(r##"
//! # namespace example {};
//! # enum Example {
//! #   "one",
//! #   "two"
//! # };
//! # "##, "crate_name")?;
//! let e = ci.get_enum_definition("Example").unwrap();
//! assert_eq!(e.name(), "Example");
//! assert_eq!(e.variants().len(), 2);
//! assert_eq!(e.variants()[0].name(), "one");
//! assert_eq!(e.variants()[1].name(), "two");
//! # Ok::<(), anyhow::Error>(())
//! ```
//!
//! Like in Rust, UniFFI enums can contain associated data, but this needs to be
//! declared with a different syntax in order to work within the restrictions of
//! WebIDL. A declaration like this:
//!
//! ```
//! # let ci = uniffi_bindgen::interface::ComponentInterface::from_webidl(r##"
//! # namespace example {};
//! [Enum]
//! interface Example {
//!   Zero();
//!   One(u32 first);
//!   Two(u32 first, string second);
//! };
//! # "##, "crate_name")?;
//! # Ok::<(), anyhow::Error>(())
//! ```
//!
//! Will result in an [`Enum`] member whose variants have associated fields:
//!
//! ```
//! # let ci = uniffi_bindgen::interface::ComponentInterface::from_webidl(r##"
//! # namespace example {};
//! # [Enum]
//! # interface ExampleWithData {
//! #   Zero();
//! #   One(u32 first);
//! #   Two(u32 first, string second);
//! # };
//! # "##, "crate_name")?;
//! let e = ci.get_enum_definition("ExampleWithData").unwrap();
//! assert_eq!(e.name(), "ExampleWithData");
//! assert_eq!(e.variants().len(), 3);
//! assert_eq!(e.variants()[0].name(), "Zero");
//! assert_eq!(e.variants()[0].fields().len(), 0);
//! assert_eq!(e.variants()[1].name(), "One");
//! assert_eq!(e.variants()[1].fields().len(), 1);
//! assert_eq!(e.variants()[1].fields()[0].name(), "first");
//! # Ok::<(), anyhow::Error>(())
//! ```
//!
//! # Enums are also used to represent error definitions for a `ComponentInterface`.
//!
//! ```
//! # let ci = uniffi_bindgen::interface::ComponentInterface::from_webidl(r##"
//! # namespace example {};
//! [Error]
//! enum Example {
//!   "one",
//!   "two"
//! };
//! # "##, "crate_name")?;
//! # Ok::<(), anyhow::Error>(())
//! ```
//!
//! Will result in an [`Enum`] member with fieldless variants being added to the resulting [`crate::ComponentInterface`]:
//!
//! ```
//! # let ci = uniffi_bindgen::interface::ComponentInterface::from_webidl(r##"
//! # namespace example {
//! #   [Throws=Example] void func();
//! # };
//! # [Error]
//! # enum Example {
//! #   "one",
//! #   "two"
//! # };
//! # "##, "crate_name")?;
//! let err = ci.get_enum_definition("Example").unwrap();
//! assert_eq!(err.name(), "Example");
//! assert_eq!(err.variants().len(), 2);
//! assert_eq!(err.variants()[0].name(), "one");
//! assert_eq!(err.variants()[1].name(), "two");
//! assert_eq!(err.is_flat(), true);
//! assert!(ci.is_name_used_as_error(&err.name()));
//! # Ok::<(), anyhow::Error>(())
//! ```
//!
//! A declaration in the UDL like this:
//!
//! ```
//! # let ci = uniffi_bindgen::interface::ComponentInterface::from_webidl(r##"
//! # namespace example {};
//! [Error]
//! interface Example {
//!   one(i16 code);
//!   two(string reason);
//!   three(i32 x, i32 y);
//! };
//! # "##, "crate_name")?;
//! # Ok::<(), anyhow::Error>(())
//! ```
//!
//! Will result in an [`Enum`] member with variants that have fields being added to the resulting [`crate::ComponentInterface`]:
//!
//! ```
//! # let ci = uniffi_bindgen::interface::ComponentInterface::from_webidl(r##"
//! # namespace example {
//! #   [Throws=Example] void func();
//! # };
//! # [Error]
//! # interface Example {
//! #   one();
//! #   two(string reason);
//! #   three(i32 x, i32 y);
//! # };
//! # "##, "crate_name")?;
//! let err = ci.get_enum_definition("Example").unwrap();
//! assert_eq!(err.name(), "Example");
//! assert_eq!(err.variants().len(), 3);
//! assert_eq!(err.variants()[0].name(), "one");
//! assert_eq!(err.variants()[1].name(), "two");
//! assert_eq!(err.variants()[2].name(), "three");
//! assert_eq!(err.variants()[0].fields().len(), 0);
//! assert_eq!(err.variants()[1].fields().len(), 1);
//! assert_eq!(err.variants()[1].fields()[0].name(), "reason");
//! assert_eq!(err.variants()[2].fields().len(), 2);
//! assert_eq!(err.variants()[2].fields()[0].name(), "x");
//! assert_eq!(err.variants()[2].fields()[1].name(), "y");
//! assert_eq!(err.is_flat(), false);
//! assert!(ci.is_name_used_as_error(err.name()));
//! # Ok::<(), anyhow::Error>(())
//! ```

use anyhow::Result;
use uniffi_meta::{Checksum, EnumShape};

use super::function::Callable;
use super::record::Field;
use super::{
    AsType, Constructor, FfiFunction, Literal, Method, Type, TypeIterator, UniffiTrait,
    UniffiTraitMethods,
};

/// Represents an enum with named variants, each of which may have named
/// and typed fields.
///
/// Enums are passed across the FFI by serializing to a bytebuffer, with a
/// i32 indicating the variant followed by the serialization of each field.
#[derive(Debug, Clone, Checksum)]
pub struct Enum {
    pub(super) name: String,
    pub(super) module_path: String,
    pub(super) remote: bool,
    pub(super) discr_type: Option<Type>,
    pub(super) variants: Vec<Variant>,
    pub(super) shape: EnumShape,
    pub(super) non_exhaustive: bool,
    pub(super) constructors: Vec<Constructor>,
    pub(super) methods: Vec<Method>,
    uniffi_traits: Vec<UniffiTrait>,
    #[checksum_ignore]
    pub(super) docstring: Option<String>,
}

impl Enum {
    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn rename(&mut self, name: String) {
        self.name = name;
    }

    pub fn remote(&self) -> bool {
        self.remote
    }

    pub fn variants(&self) -> &[Variant] {
        &self.variants
    }

    pub fn constructors(&self) -> &[Constructor] {
        &self.constructors
    }

    pub fn methods(&self) -> &[Method] {
        &self.methods
    }

    pub fn variant_discr(&self, variant_index: usize) -> Result<Literal> {
        for (i, lit) in self.variant_discr_iter().enumerate() {
            let lit = lit?;
            if i == variant_index {
                return Ok(lit);
            }
        }
        anyhow::bail!("Invalid variant index {variant_index}");
    }

    fn variant_discr_iter(&self) -> impl Iterator<Item = Result<Literal>> + '_ {
        let mut next = 0;
        self.variants().iter().map(move |v| {
            let (this, this_lit) = match v.discr {
                None => (
                    next,
                    if (next as i64) < 0 {
                        Literal::new_int(next as i64)
                    } else {
                        Literal::new_uint(next)
                    },
                ),
                Some(Literal::UInt(v, _, _)) => (v, Literal::new_uint(v)),
                Some(Literal::Int(v, _, _)) => (v as u64, Literal::new_int(v)),
                _ => anyhow::bail!("Invalid literal type {v:?}"),
            };
            next = this.wrapping_add(1);
            Ok(this_lit)
        })
    }

    pub fn variant_discr_type(&self) -> &Option<Type> {
        &self.discr_type
    }

    pub fn is_flat(&self) -> bool {
        match self.shape {
            EnumShape::Error { flat } => flat,
            EnumShape::Enum => self.variants.iter().all(|v| v.fields.is_empty()),
        }
    }

    pub fn is_non_exhaustive(&self) -> bool {
        self.non_exhaustive
    }

    pub fn iter_types(&self) -> TypeIterator<'_> {
        Box::new(
            self.variants
                .iter()
                .flat_map(Variant::iter_types)
                .chain(self.constructors.iter().flat_map(Constructor::iter_types))
                .chain(self.methods.iter().flat_map(Method::iter_types)),
        )
    }

    pub fn docstring(&self) -> Option<&str> {
        self.docstring.as_deref()
    }

    pub fn contains_variant_fields(&self) -> bool {
        self.variants().iter().any(|v| v.has_fields())
    }

    pub fn uniffi_trait_methods(&self) -> UniffiTraitMethods {
        UniffiTraitMethods::new(&self.uniffi_traits)
    }

    pub fn add_uniffi_trait(&mut self, t: UniffiTrait) {
        self.uniffi_traits.push(t);
    }

    pub fn derive_ffi_funcs(&mut self) -> Result<()> {
        for c in self.constructors.iter_mut() {
            c.derive_ffi_func();
        }
        for m in self.methods.iter_mut() {
            m.derive_ffi_func()?;
        }
        for ut in self.uniffi_traits.iter_mut() {
            ut.derive_ffi_func()?;
        }
        Ok(())
    }

    pub fn iter_ffi_function_definitions(&self) -> impl Iterator<Item = &FfiFunction> {
        self.constructors
            .iter()
            .map(|f| &f.ffi_func)
            .chain(self.methods.iter().map(|f| &f.ffi_func))
            .chain(
                self.uniffi_traits
                    .iter()
                    .flat_map(|ut| match ut {
                        UniffiTrait::Display { fmt: m }
                        | UniffiTrait::Debug { fmt: m }
                        | UniffiTrait::Hash { hash: m }
                        | UniffiTrait::Ord { cmp: m } => vec![m],
                        UniffiTrait::Eq { eq, ne } => vec![eq, ne],
                    })
                    .map(|m| &m.ffi_func),
            )
    }
}

impl TryFrom<uniffi_meta::EnumMetadata> for Enum {
    type Error = anyhow::Error;

    fn try_from(meta: uniffi_meta::EnumMetadata) -> Result<Self> {
        Ok(Self {
            name: meta.name,
            module_path: meta.module_path,
            remote: meta.remote,
            discr_type: meta.discr_type,
            variants: meta
                .variants
                .into_iter()
                .map(TryInto::try_into)
                .collect::<Result<_>>()?,
            shape: meta.shape,
            non_exhaustive: meta.non_exhaustive,
            constructors: vec![],
            methods: vec![],
            uniffi_traits: vec![],
            docstring: meta.docstring.clone(),
        })
    }
}

impl AsType for Enum {
    fn as_type(&self) -> Type {
        Type::Enum {
            name: self.name.clone(),
            module_path: self.module_path.clone(),
        }
    }
}

/// Represents an individual variant in an Enum.
///
/// Each variant has a name and zero or more fields.
#[derive(Debug, Clone, Default, PartialEq, Eq, Checksum)]
pub struct Variant {
    pub(super) name: String,
    pub(super) discr: Option<Literal>,
    pub(super) fields: Vec<Field>,
    #[checksum_ignore]
    pub(super) docstring: Option<String>,
}

impl Variant {
    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn rename(&mut self, new_name: String) {
        self.name = new_name;
    }

    pub fn fields(&self) -> &[Field] {
        &self.fields
    }

    pub fn has_fields(&self) -> bool {
        !self.fields.is_empty()
    }

    pub fn has_nameless_fields(&self) -> bool {
        self.fields.iter().any(|f| f.name.is_empty())
    }

    pub fn docstring(&self) -> Option<&str> {
        self.docstring.as_deref()
    }

    pub fn iter_types(&self) -> TypeIterator<'_> {
        Box::new(self.fields.iter().flat_map(Field::iter_types))
    }
}

impl TryFrom<uniffi_meta::VariantMetadata> for Variant {
    type Error = anyhow::Error;

    fn try_from(meta: uniffi_meta::VariantMetadata) -> Result<Self> {
        Ok(Self {
            name: meta.name,
            discr: meta.discr,
            fields: meta
                .fields
                .into_iter()
                .map(TryInto::try_into)
                .collect::<Result<_>>()?,
            docstring: meta.docstring.clone(),
        })
    }
}
