/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use super::APIConverter;
use crate::{attributes::EnumAttributes, converters::convert_docstring, InterfaceCollector};
use anyhow::{bail, Result};

use uniffi_meta::{EnumMetadata, EnumShape, Type, VariantMetadata};

impl APIConverter<EnumMetadata> for weedle::EnumDefinition<'_> {
    fn convert(&self, ci: &mut InterfaceCollector) -> Result<EnumMetadata> {
        let attributes = EnumAttributes::try_from(self.attributes.as_ref())?;
        let shape = if attributes.contains_error_attr() {
            EnumShape::Error { flat: true }
        } else {
            EnumShape::Enum
        };
        Ok(EnumMetadata {
            module_path: ci.module_path(),
            name: self.identifier.0.to_string(),
            shape,
            remote: attributes.contains_remote(),
            discr_type: None,
            variants: self
                .values
                .body
                .list
                .iter()
                .map::<Result<_>, _>(|v| {
                    Ok(VariantMetadata {
                        name: v.value.0.to_string(),
                        discr: None,
                        fields: vec![],
                        docstring: v.docstring.as_ref().map(|v| convert_docstring(&v.0)),
                    })
                })
                .collect::<Result<Vec<_>>>()?,
            non_exhaustive: attributes.contains_non_exhaustive_attr(),
            docstring: self.docstring.as_ref().map(|v| convert_docstring(&v.0)),
        })
    }
}

impl APIConverter<EnumMetadata> for weedle::InterfaceDefinition<'_> {
    fn convert(&self, ci: &mut InterfaceCollector) -> Result<EnumMetadata> {
        if self.inheritance.is_some() {
            bail!("interface inheritance is not supported for enum interfaces");
        }
        let attributes = EnumAttributes::try_from(self.attributes.as_ref())?;
        let shape = if attributes.contains_error_attr() {
            EnumShape::Error { flat: false }
        } else {
            EnumShape::Enum
        };
        let other = Type::Enum {
            module_path: ci.module_path().to_string(),
            name: self.identifier.0.to_string(),
        };

        for ut in super::make_uniffi_traits(
            &ci.module_path(),
            self.identifier.0,
            &attributes.get_uniffi_traits(),
            &other,
        )? {
            ci.items.insert(ut.into());
        }

        Ok(EnumMetadata {
            module_path: ci.module_path(),
            name: self.identifier.0.to_string(),
            shape,
            remote: attributes.contains_remote(),
            variants: self
                .members
                .body
                .iter()
                .map::<Result<VariantMetadata>, _>(|member| match member {
                    weedle::interface::InterfaceMember::Operation(t) => Ok(t.convert(ci)?),
                    _ => bail!(
                        "interface member type {:?} not supported in enum interface",
                        member
                    ),
                })
                .collect::<Result<Vec<_>>>()?,
            discr_type: None,
            non_exhaustive: attributes.contains_non_exhaustive_attr(),
            docstring: self.docstring.as_ref().map(|v| convert_docstring(&v.0)),
        })
    }
}
