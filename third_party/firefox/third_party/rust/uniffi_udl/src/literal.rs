/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use anyhow::{bail, Result};
use uniffi_meta::{DefaultValueMetadata, LiteralMetadata, Radix, Type};

pub type Literal = LiteralMetadata;

pub(super) fn convert_default_value(
    default_value: &weedle::literal::DefaultValue<'_>,
    type_: &Type,
) -> Result<LiteralMetadata> {
    fn convert_integer(literal: &weedle::literal::IntegerLit<'_>, type_: &Type) -> Result<Literal> {
        let (string, radix) = match literal {
            weedle::literal::IntegerLit::Dec(v) => (v.0, Radix::Decimal),
            weedle::literal::IntegerLit::Hex(v) => (v.0, Radix::Hexadecimal),
            weedle::literal::IntegerLit::Oct(v) => (v.0, Radix::Octal),
        };
        let src_radix = radix as u32;
        let dest_radix = if string == "0" || string.starts_with('-') {
            Radix::Decimal
        } else {
            radix
        };

        #[allow(clippy::manual_strip)]
        let string = if string.starts_with('-') {
            ("-".to_string() + string[1..].trim_start_matches("0x")).to_lowercase()
        } else {
            string.trim_start_matches("0x").to_lowercase()
        };

        Ok(match type_ {
            Type::Int8 | Type::Int16 | Type::Int32 | Type::Int64 => Literal::Int(
                i64::from_str_radix(&string, src_radix)?,
                dest_radix,
                type_.clone(),
            ),
            Type::UInt8 | Type::UInt16 | Type::UInt32 | Type::UInt64 => Literal::UInt(
                u64::from_str_radix(&string, src_radix)?,
                dest_radix,
                type_.clone(),
            ),

            _ => bail!("Cannot coerce literal {} into a non-integer type", string),
        })
    }

    fn convert_float(literal: &weedle::literal::FloatLit<'_>, type_: &Type) -> Result<Literal> {
        let string = match literal {
            weedle::literal::FloatLit::Value(v) => v.0,

            _ => bail!("Infinity and NaN is not currently supported"),
        };

        Ok(match type_ {
            Type::Float32 | Type::Float64 => Literal::Float(string.to_string(), type_.clone()),
            _ => bail!("Cannot coerce literal {} into a non-float type", string),
        })
    }

    Ok(match (default_value, type_) {
        (weedle::literal::DefaultValue::Boolean(b), Type::Boolean) => Literal::Boolean(b.0),
        (weedle::literal::DefaultValue::String(s), Type::String) => {
            Literal::String(s.0.to_string())
        }
        (weedle::literal::DefaultValue::EmptyArray(_), Type::Sequence { .. }) => {
            Literal::EmptySequence
        }
        (weedle::literal::DefaultValue::EmptyDictionary(_), Type::Map { .. }) => Literal::EmptyMap,
        (weedle::literal::DefaultValue::String(s), Type::Enum { .. }) => {
            Literal::Enum(s.0.to_string(), type_.clone())
        }
        (weedle::literal::DefaultValue::Null(_), Type::Optional { .. }) => Literal::None,
        (_, Type::Optional { inner_type, .. }) => Literal::Some {
            inner: Box::new(DefaultValueMetadata::Literal(convert_default_value(
                default_value,
                inner_type,
            )?)),
        },

        (weedle::literal::DefaultValue::Integer(i), _) => convert_integer(i, type_)?,
        (weedle::literal::DefaultValue::Float(i), _) => convert_float(i, type_)?,

        _ => bail!("No support for {:?} literal yet", default_value),
    })
}
