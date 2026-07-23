/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! # Attribute definitions for a `InterfaceCollector`.
//!
//! This module provides some conveniences for working with attribute definitions
//! from WebIDL. When encountering a weedle `ExtendedAttribute` node, use `TryFrom`
//! to convert it into an [`Attribute`] representing one of the attributes that we
//! support. You can also use the [`parse_attributes`] function to parse an
//! `ExtendedAttributeList` into a vec of same.
//!
//! We only support a small number of attributes, so it's manageable to have them
//! all handled by a single abstraction. This might need to be refactored in future
//! if we grow significantly more complicated attribute handling.

use anyhow::{bail, Result};
use uniffi_meta::{Checksum, ObjectImpl};

/// Represents an attribute parsed from UDL, like `[ByRef]` or `[Throws]`.
///
/// This is a convenience enum for parsing UDL attributes and erroring out if we encounter
/// any unsupported ones. These don't convert directly into parts of a `InterfaceCollector`, but
/// may influence the properties of things like functions and arguments.
#[derive(Debug, Clone, Checksum)]
pub(super) enum Attribute {
    ByRef,
    Enum,
    Error,
    Name(String),
    SelfType(SelfType),
    Throws(String),
    Traits(Vec<String>),
    External { crate_name: String },
    Remote,
    Custom { crate_name: Option<String> },
    Trait,
    WithForeign,
    Async,
    NonExhaustive,
}

impl Attribute {
    pub fn is_error(&self) -> bool {
        matches!(self, Attribute::Error)
    }
    pub fn is_enum(&self) -> bool {
        matches!(self, Attribute::Enum)
    }
}

/// Convert a weedle `ExtendedAttribute` into an `Attribute` for a `InterfaceCollector` member,
/// or error out if the attribute is not supported.
impl TryFrom<&weedle::attribute::ExtendedAttribute<'_>> for Attribute {
    type Error = anyhow::Error;
    fn try_from(
        weedle_attribute: &weedle::attribute::ExtendedAttribute<'_>,
    ) -> Result<Self, anyhow::Error> {
        match weedle_attribute {
            weedle::attribute::ExtendedAttribute::NoArgs(attr) => match (attr.0).0 {
                "ByRef" => Ok(Attribute::ByRef),
                "Enum" => Ok(Attribute::Enum),
                "Error" => Ok(Attribute::Error),
                "Custom" => Ok(Attribute::Custom { crate_name: None }),
                "Trait" => Ok(Attribute::Trait),
                "WithForeign" => Ok(Attribute::WithForeign),
                "Async" => Ok(Attribute::Async),
                "NonExhaustive" => Ok(Attribute::NonExhaustive),
                "Remote" => Ok(Attribute::Remote),
                _ => anyhow::bail!("ExtendedAttributeNoArgs not supported: {:?}", (attr.0).0),
            },
            weedle::attribute::ExtendedAttribute::Ident(identity) => {
                match identity.lhs_identifier.0 {
                    "Name" => Ok(Attribute::Name(name_from_id_or_string(&identity.rhs))),
                    "Throws" => Ok(Attribute::Throws(name_from_id_or_string(&identity.rhs))),
                    "Self" => Ok(Attribute::SelfType(SelfType::try_from(&identity.rhs)?)),
                    "External" => Ok(Attribute::External {
                        crate_name: name_from_id_or_string(&identity.rhs),
                    }),
                    "Custom" => Ok(Attribute::Custom {
                        crate_name: Some(name_from_id_or_string(&identity.rhs)),
                    }),
                    _ => anyhow::bail!(
                        "Attribute identity Identifier not supported: {:?}",
                        identity.lhs_identifier.0
                    ),
                }
            }
            weedle::attribute::ExtendedAttribute::IdentList(attr_list) => {
                match attr_list.identifier.0 {
                    "Traits" => Ok(Attribute::Traits(
                        attr_list
                            .list
                            .body
                            .list
                            .iter()
                            .map(|i| i.0.to_string())
                            .collect(),
                    )),
                    _ => anyhow::bail!(
                        "Attribute identity list not supported: {:?}",
                        attr_list.identifier.0
                    ),
                }
            }
            _ => anyhow::bail!("Attribute not supported: {:?}", weedle_attribute),
        }
    }
}

fn name_from_id_or_string(nm: &weedle::attribute::IdentifierOrString<'_>) -> String {
    match nm {
        weedle::attribute::IdentifierOrString::Identifier(identifier) => identifier.0.to_string(),
        weedle::attribute::IdentifierOrString::String(str_lit) => str_lit.0.to_string(),
    }
}

/// Parse a weedle `ExtendedAttributeList` into a list of `Attribute`s,
/// erroring out on duplicates.
fn parse_attributes<F>(
    weedle_attributes: &weedle::attribute::ExtendedAttributeList<'_>,
    validator: F,
) -> Result<Vec<Attribute>>
where
    F: Fn(&Attribute) -> Result<()>,
{
    let attrs = &weedle_attributes.body.list;

    let mut hash_set = std::collections::HashSet::new();
    for attr in attrs {
        if !hash_set.insert(attr) {
            anyhow::bail!("Duplicated ExtendedAttribute: {:?}", attr);
        }
    }

    let attrs = attrs
        .iter()
        .map(Attribute::try_from)
        .collect::<Result<Vec<_>, _>>()?;

    for attr in &attrs {
        validator(attr)?;
    }

    Ok(attrs)
}

/// Attributes that can be attached to an `dictionary` definition in the UDL.
#[derive(Debug, Clone, Checksum, Default)]
pub(super) struct DictionaryAttributes(Vec<Attribute>);

impl DictionaryAttributes {
    pub fn contains_remote(&self) -> bool {
        self.0.iter().any(|attr| matches!(attr, Attribute::Remote))
    }

    pub fn get_uniffi_traits(&self) -> Vec<String> {
        self.0
            .iter()
            .find_map(|attr| match attr {
                Attribute::Traits(inner) => Some(inner.clone()),
                _ => None,
            })
            .unwrap_or_default()
    }
}

impl TryFrom<&weedle::attribute::ExtendedAttributeList<'_>> for DictionaryAttributes {
    type Error = anyhow::Error;
    fn try_from(
        weedle_attributes: &weedle::attribute::ExtendedAttributeList<'_>,
    ) -> Result<Self, Self::Error> {
        let attrs = parse_attributes(weedle_attributes, |attr| match attr {
            Attribute::Remote => Ok(()),
            Attribute::Traits(_) => Ok(()),
            _ => bail!(format!("{attr:?} not supported for dictionaries")),
        })?;
        Ok(Self(attrs))
    }
}

impl<T: TryInto<DictionaryAttributes, Error = anyhow::Error>> TryFrom<Option<T>>
    for DictionaryAttributes
{
    type Error = anyhow::Error;
    fn try_from(value: Option<T>) -> Result<Self, Self::Error> {
        match value {
            None => Ok(Default::default()),
            Some(v) => v.try_into(),
        }
    }
}

/// Attributes that can be attached to an `enum` definition in the UDL.
#[derive(Debug, Clone, Checksum, Default)]
pub(super) struct EnumAttributes(Vec<Attribute>);

impl EnumAttributes {
    pub fn contains_error_attr(&self) -> bool {
        self.0.iter().any(|attr| attr.is_error())
    }

    pub fn contains_non_exhaustive_attr(&self) -> bool {
        self.0
            .iter()
            .any(|attr| matches!(attr, Attribute::NonExhaustive))
    }

    pub fn contains_remote(&self) -> bool {
        self.0.iter().any(|attr| matches!(attr, Attribute::Remote))
    }

    pub fn get_uniffi_traits(&self) -> Vec<String> {
        self.0
            .iter()
            .find_map(|attr| match attr {
                Attribute::Traits(inner) => Some(inner.clone()),
                _ => None,
            })
            .unwrap_or_default()
    }
}

impl TryFrom<&weedle::attribute::ExtendedAttributeList<'_>> for EnumAttributes {
    type Error = anyhow::Error;
    fn try_from(
        weedle_attributes: &weedle::attribute::ExtendedAttributeList<'_>,
    ) -> Result<Self, Self::Error> {
        let attrs = parse_attributes(weedle_attributes, |attr| match attr {
            Attribute::Error => Ok(()),
            Attribute::NonExhaustive => Ok(()),
            Attribute::Remote => Ok(()),
            Attribute::Enum => Ok(()),
            Attribute::Traits(_) => Ok(()),
            _ => bail!(format!("{attr:?} not supported for enums")),
        })?;
        Ok(Self(attrs))
    }
}

impl<T: TryInto<EnumAttributes, Error = anyhow::Error>> TryFrom<Option<T>> for EnumAttributes {
    type Error = anyhow::Error;
    fn try_from(value: Option<T>) -> Result<Self, Self::Error> {
        match value {
            None => Ok(Default::default()),
            Some(v) => v.try_into(),
        }
    }
}

/// Represents UDL attributes that might appear on a function.
///
/// This supports:
///   * `[Throws=ErrorName]` attribute for functions that can produce an error.
///   * `[Async] for async functions
#[derive(Debug, Clone, Checksum, Default)]
pub(super) struct FunctionAttributes(Vec<Attribute>);

impl FunctionAttributes {
    pub(super) fn get_throws_err(&self) -> Option<&str> {
        self.0.iter().find_map(|attr| match attr {
            Attribute::Throws(inner) => Some(inner.as_ref()),
            _ => None,
        })
    }

    pub(super) fn is_async(&self) -> bool {
        self.0.iter().any(|attr| matches!(attr, Attribute::Async))
    }
}

impl FromIterator<Attribute> for FunctionAttributes {
    fn from_iter<T: IntoIterator<Item = Attribute>>(iter: T) -> Self {
        Self(Vec::from_iter(iter))
    }
}

impl TryFrom<&weedle::attribute::ExtendedAttributeList<'_>> for FunctionAttributes {
    type Error = anyhow::Error;
    fn try_from(
        weedle_attributes: &weedle::attribute::ExtendedAttributeList<'_>,
    ) -> Result<Self, Self::Error> {
        let attrs = parse_attributes(weedle_attributes, |attr| match attr {
            Attribute::Throws(_) | Attribute::Async => Ok(()),
            _ => bail!(format!("{attr:?} not supported for functions")),
        })?;
        Ok(Self(attrs))
    }
}

impl<T: TryInto<FunctionAttributes, Error = anyhow::Error>> TryFrom<Option<T>>
    for FunctionAttributes
{
    type Error = anyhow::Error;
    fn try_from(value: Option<T>) -> Result<Self, Self::Error> {
        match value {
            None => Ok(Default::default()),
            Some(v) => v.try_into(),
        }
    }
}

/// Represents UDL attributes that might appear on a function argument.
///
/// This supports the `[ByRef]` attribute for arguments that should be passed
/// by reference in the generated Rust scaffolding.
#[derive(Debug, Clone, Checksum, Default)]
pub(super) struct ArgumentAttributes(Vec<Attribute>);

impl ArgumentAttributes {
    pub fn by_ref(&self) -> bool {
        self.0.iter().any(|attr| matches!(attr, Attribute::ByRef))
    }
}

impl TryFrom<&weedle::attribute::ExtendedAttributeList<'_>> for ArgumentAttributes {
    type Error = anyhow::Error;
    fn try_from(
        weedle_attributes: &weedle::attribute::ExtendedAttributeList<'_>,
    ) -> Result<Self, Self::Error> {
        let attrs = parse_attributes(weedle_attributes, |attr| match attr {
            Attribute::ByRef => Ok(()),
            _ => bail!(format!("{attr:?} not supported for arguments")),
        })?;
        Ok(Self(attrs))
    }
}

impl<T: TryInto<ArgumentAttributes, Error = anyhow::Error>> TryFrom<Option<T>>
    for ArgumentAttributes
{
    type Error = anyhow::Error;
    fn try_from(value: Option<T>) -> Result<Self, Self::Error> {
        match value {
            None => Ok(Default::default()),
            Some(v) => v.try_into(),
        }
    }
}

/// Represents UDL attributes that might appear on an `interface` definition.
#[derive(Debug, Clone, Checksum, Default)]
pub(super) struct InterfaceAttributes(Vec<Attribute>);

impl InterfaceAttributes {
    pub fn contains_enum_attr(&self) -> bool {
        self.0.iter().any(|attr| attr.is_enum())
    }

    pub fn contains_error_attr(&self) -> bool {
        self.0.iter().any(|attr| attr.is_error())
    }

    pub fn contains_trait(&self) -> bool {
        self.0.iter().any(|attr| matches!(attr, Attribute::Trait))
    }

    pub fn contains_remote(&self) -> bool {
        self.0.iter().any(|attr| matches!(attr, Attribute::Remote))
    }

    pub fn contains_with_foreign(&self) -> bool {
        self.0
            .iter()
            .any(|attr| matches!(attr, Attribute::WithForeign))
    }

    pub fn object_impl(&self) -> Result<ObjectImpl> {
        Ok(
            match (self.contains_trait(), self.contains_with_foreign()) {
                (true, true) => ObjectImpl::CallbackTrait,
                (true, false) => ObjectImpl::Trait,
                (false, false) => ObjectImpl::Struct,
                (false, true) => bail!("WithForeign can't be specified without Trait"),
            },
        )
    }
    pub fn get_traits(&self) -> Vec<String> {
        self.0
            .iter()
            .find_map(|attr| match attr {
                Attribute::Traits(inner) => Some(inner.clone()),
                _ => None,
            })
            .unwrap_or_default()
    }
}

impl TryFrom<&weedle::attribute::ExtendedAttributeList<'_>> for InterfaceAttributes {
    type Error = anyhow::Error;
    fn try_from(
        weedle_attributes: &weedle::attribute::ExtendedAttributeList<'_>,
    ) -> Result<Self, Self::Error> {
        let attrs = parse_attributes(weedle_attributes, |attr| match attr {
            Attribute::Enum => Ok(()),
            Attribute::Error => Ok(()),
            Attribute::Trait => Ok(()),
            Attribute::WithForeign => Ok(()),
            Attribute::Traits(_) => Ok(()),
            Attribute::Remote => Ok(()),
            _ => bail!(format!("{attr:?} not supported for interface definition")),
        })?;
        let ok_for_enum =
            attrs.len() == 1 || attrs.iter().any(|a| matches!(a, Attribute::Traits(_)));
        if attrs.iter().any(|a| matches!(a, Attribute::Enum)) && !ok_for_enum {
            bail!("conflicting attributes on interface definition");
        }
        Ok(Self(attrs))
    }
}

impl<T: TryInto<InterfaceAttributes, Error = anyhow::Error>> TryFrom<Option<T>>
    for InterfaceAttributes
{
    type Error = anyhow::Error;
    fn try_from(value: Option<T>) -> Result<Self, Self::Error> {
        match value {
            None => Ok(Default::default()),
            Some(v) => v.try_into(),
        }
    }
}

/// Represents UDL attributes that might appear on a constructor.
///
/// This supports the `[Throws=ErrorName]` attribute for constructors that can produce
/// an error, and the `[Name=MethodName]` for non-default constructors.
#[derive(Debug, Clone, Checksum, Default)]
pub(super) struct ConstructorAttributes(Vec<Attribute>);

impl FromIterator<Attribute> for ConstructorAttributes {
    fn from_iter<T: IntoIterator<Item = Attribute>>(iter: T) -> Self {
        Self(Vec::from_iter(iter))
    }
}

impl ConstructorAttributes {
    pub(super) fn get_throws_err(&self) -> Option<&str> {
        self.0.iter().find_map(|attr| match attr {
            Attribute::Throws(inner) => Some(inner.as_ref()),
            _ => None,
        })
    }

    pub(super) fn get_name(&self) -> Option<&str> {
        self.0.iter().find_map(|attr| match attr {
            Attribute::Name(inner) => Some(inner.as_ref()),
            _ => None,
        })
    }

    pub(super) fn is_async(&self) -> bool {
        self.0.iter().any(|attr| matches!(attr, Attribute::Async))
    }
}

impl TryFrom<&weedle::attribute::ExtendedAttributeList<'_>> for ConstructorAttributes {
    type Error = anyhow::Error;
    fn try_from(
        weedle_attributes: &weedle::attribute::ExtendedAttributeList<'_>,
    ) -> Result<Self, Self::Error> {
        let attrs = parse_attributes(weedle_attributes, |attr| match attr {
            Attribute::Throws(_) => Ok(()),
            Attribute::Name(_) => Ok(()),
            Attribute::Async => Ok(()),
            _ => bail!(format!("{attr:?} not supported for constructors")),
        })?;
        Ok(Self(attrs))
    }
}

/// Represents UDL attributes that might appear on a method.
///
/// This supports the `[Throws=ErrorName]` attribute for methods that can produce
/// an error, and the `[Self=ByArc]` attribute for methods that take `Arc<Self>` as receiver.
#[derive(Debug, Clone, Checksum, Default)]
pub(super) struct MethodAttributes(Vec<Attribute>);

impl MethodAttributes {
    pub(super) fn get_throws_err(&self) -> Option<&str> {
        self.0.iter().find_map(|attr| match attr {
            Attribute::Throws(inner) => Some(inner.as_ref()),
            _ => None,
        })
    }

    pub(super) fn is_async(&self) -> bool {
        self.0.iter().any(|attr| matches!(attr, Attribute::Async))
    }

    pub(super) fn get_self_by_arc(&self) -> bool {
        self.0
            .iter()
            .any(|attr| matches!(attr, Attribute::SelfType(SelfType::ByArc)))
    }
}

impl FromIterator<Attribute> for MethodAttributes {
    fn from_iter<T: IntoIterator<Item = Attribute>>(iter: T) -> Self {
        Self(Vec::from_iter(iter))
    }
}

impl TryFrom<&weedle::attribute::ExtendedAttributeList<'_>> for MethodAttributes {
    type Error = anyhow::Error;
    fn try_from(
        weedle_attributes: &weedle::attribute::ExtendedAttributeList<'_>,
    ) -> Result<Self, Self::Error> {
        let attrs = parse_attributes(weedle_attributes, |attr| match attr {
            Attribute::SelfType(_) | Attribute::Throws(_) | Attribute::Async => Ok(()),
            _ => bail!(format!("{attr:?} not supported for methods")),
        })?;
        Ok(Self(attrs))
    }
}

impl<T: TryInto<MethodAttributes, Error = anyhow::Error>> TryFrom<Option<T>> for MethodAttributes {
    type Error = anyhow::Error;
    fn try_from(value: Option<T>) -> Result<Self, Self::Error> {
        match value {
            None => Ok(Default::default()),
            Some(v) => v.try_into(),
        }
    }
}

/// Represents the different possible types of method call receiver.
///
/// Actually we only support one of these right now, `[Self=ByArc]`.
/// We might add more in future, e.g. a `[Self=ByRef]` if there are cases
/// where we need to force the receiver to be taken by reference.
#[derive(Debug, Clone, Checksum)]
pub(super) enum SelfType {
    ByArc, 
}

impl TryFrom<&weedle::attribute::IdentifierOrString<'_>> for SelfType {
    type Error = anyhow::Error;
    fn try_from(nm: &weedle::attribute::IdentifierOrString<'_>) -> Result<Self, Self::Error> {
        Ok(match nm {
            weedle::attribute::IdentifierOrString::Identifier(identifier) => match identifier.0 {
                "ByArc" => SelfType::ByArc,
                _ => bail!("Unsupported Self Type: {:?}", identifier.0),
            },
            weedle::attribute::IdentifierOrString::String(_) => {
                bail!("Unsupported Self Type: {:?}", nm)
            }
        })
    }
}

/// Represents UDL attributes that might appear on a typedef
///
/// This supports the `[External="crate_name"]` and `[Custom]` attributes for types.
#[derive(Debug, Clone, Checksum, Default)]
pub(super) struct TypedefAttributes(Vec<Attribute>);

impl TypedefAttributes {
    pub(super) fn get_crate_name(&self) -> Option<String> {
        self.0.iter().find_map(|attr| match attr {
            Attribute::External { crate_name, .. } => Some(crate_name.clone()),
            Attribute::Custom {
                crate_name: Some(crate_name),
                ..
            } => Some(crate_name.clone()),
            _ => None,
        })
    }

    pub(super) fn is_custom(&self) -> bool {
        self.0
            .iter()
            .any(|attr| matches!(attr, Attribute::Custom { .. }))
    }
}

impl TryFrom<&weedle::attribute::ExtendedAttributeList<'_>> for TypedefAttributes {
    type Error = anyhow::Error;
    fn try_from(
        weedle_attributes: &weedle::attribute::ExtendedAttributeList<'_>,
    ) -> Result<Self, Self::Error> {
        let attrs = parse_attributes(weedle_attributes, |attr| match attr {
            Attribute::External { .. } | Attribute::Custom { .. } => Ok(()),
            _ => bail!(format!("{attr:?} not supported for typedefs")),
        })?;
        if attrs.len() > 1 {
            bail!("Can't be [Custom] and [External]");
        }
        Ok(Self(attrs))
    }
}

impl<T: TryInto<TypedefAttributes, Error = anyhow::Error>> TryFrom<Option<T>>
    for TypedefAttributes
{
    type Error = anyhow::Error;
    fn try_from(value: Option<T>) -> Result<Self, Self::Error> {
        match value {
            None => Ok(Default::default()),
            Some(v) => v.try_into(),
        }
    }
}
