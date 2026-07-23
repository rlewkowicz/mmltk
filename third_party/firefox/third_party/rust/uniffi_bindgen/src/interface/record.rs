/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! # Record definitions for a `ComponentInterface`.
//!
//! This module converts "dictionary" definitions from UDL into [`Record`] structures
//! that can be added to a `ComponentInterface`, which are the main way we define structured
//! data types for a UniFFI Rust Component. A [`Record`] has a fixed set of named fields,
//! each of a specific type.
//!
//! (The terminology mismatch between "dictionary" and "record" is a historical artifact
//! due to this tool being loosely inspired by WebAssembly Interface Types, which used
//! the term "record" for this sort of data).
//!
//! A declaration in the UDL like this:
//!
//! ```
//! # let ci = uniffi_bindgen::interface::ComponentInterface::from_webidl(r##"
//! # namespace example {};
//! dictionary Example {
//!   string name;
//!   u32 value;
//! };
//! # "##, "crate_name")?;
//! # Ok::<(), anyhow::Error>(())
//! ```
//!
//! Will result in a [`Record`] member with two [`Field`]s being added to the resulting
//! [`crate::ComponentInterface`]:
//!
//! ```
//! # let ci = uniffi_bindgen::interface::ComponentInterface::from_webidl(r##"
//! # namespace example {};
//! # dictionary Example {
//! #   string name;
//! #   u32 value;
//! # };
//! # "##, "crate_name")?;
//! let record = ci.get_record_definition("Example").unwrap();
//! assert_eq!(record.name(), "Example");
//! assert_eq!(record.fields()[0].name(), "name");
//! assert_eq!(record.fields()[1].name(), "value");
//! # Ok::<(), anyhow::Error>(())
//! ```

use anyhow::Result;
use uniffi_meta::Checksum;

use super::function::Callable;
use super::{
    AsType, Constructor, DefaultValue, FfiFunction, Method, Type, TypeIterator, UniffiTrait,
    UniffiTraitMethods,
};

/// Represents a "data class" style object, for passing around complex values.
///
/// In the FFI these are represented as a byte buffer, which one side explicitly
/// serializes the data into and the other serializes it out of. So I guess they're
/// kind of like "pass by clone" values.
#[derive(Debug, Clone, Checksum)]
pub struct Record {
    pub(super) name: String,
    pub(super) module_path: String,
    pub(super) remote: bool,
    pub(super) fields: Vec<Field>,
    pub(super) constructors: Vec<Constructor>,
    pub(super) methods: Vec<Method>,
    pub uniffi_traits: Vec<UniffiTrait>,
    #[checksum_ignore]
    pub(super) docstring: Option<String>,
}

impl Record {
    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn rename(&mut self, name: String) {
        self.name = name;
    }

    pub fn remote(&self) -> bool {
        self.remote
    }

    pub fn fields(&self) -> &[Field] {
        &self.fields
    }

    pub fn constructors(&self) -> &[Constructor] {
        &self.constructors
    }

    pub fn methods(&self) -> &[Method] {
        &self.methods
    }

    pub fn docstring(&self) -> Option<&str> {
        self.docstring.as_deref()
    }

    pub fn iter_types(&self) -> TypeIterator<'_> {
        Box::new(
            self.fields
                .iter()
                .flat_map(Field::iter_types)
                .chain(self.constructors.iter().flat_map(Constructor::iter_types))
                .chain(self.methods.iter().flat_map(Method::iter_types)),
        )
    }

    pub fn has_fields(&self) -> bool {
        !self.fields.is_empty()
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

impl AsType for Record {
    fn as_type(&self) -> Type {
        Type::Record {
            name: self.name.clone(),
            module_path: self.module_path.clone(),
        }
    }
}

impl TryFrom<uniffi_meta::RecordMetadata> for Record {
    type Error = anyhow::Error;

    fn try_from(meta: uniffi_meta::RecordMetadata) -> Result<Self> {
        Ok(Self {
            name: meta.name,
            module_path: meta.module_path,
            remote: meta.remote,
            fields: meta
                .fields
                .into_iter()
                .map(TryInto::try_into)
                .collect::<Result<_>>()?,
            constructors: vec![],
            methods: vec![],
            uniffi_traits: vec![],
            docstring: meta.docstring.clone(),
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Checksum)]
pub struct Field {
    pub(super) name: String,
    pub(super) type_: Type,
    pub(super) default: Option<DefaultValue>,
    #[checksum_ignore]
    pub(super) docstring: Option<String>,
}

impl Field {
    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn rename(&mut self, name: String) {
        self.name = name;
    }

    pub fn default_value(&self) -> Option<&DefaultValue> {
        self.default.as_ref()
    }

    pub fn docstring(&self) -> Option<&str> {
        self.docstring.as_deref()
    }

    pub fn iter_types(&self) -> TypeIterator<'_> {
        self.type_.iter_types()
    }
}

impl AsType for Field {
    fn as_type(&self) -> Type {
        self.type_.clone()
    }
}

impl TryFrom<uniffi_meta::FieldMetadata> for Field {
    type Error = anyhow::Error;

    fn try_from(meta: uniffi_meta::FieldMetadata) -> Result<Self> {
        let name = meta.name;
        let type_ = meta.ty;
        let default = meta.default;
        Ok(Self {
            name,
            type_,
            default,
            docstring: meta.docstring.clone(),
        })
    }
}
