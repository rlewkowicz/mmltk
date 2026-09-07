use std::borrow::Cow;

pub type Object = Vec<(String, Value)>;

/// The only dynamic representation permitted across the native application
/// boundary. Byte strings remain bytes rather than being expanded into text
/// or numeric values.
#[derive(Debug, Clone)]
pub enum Value {
    Null,
    Bool(bool),
    Signed(i64),
    Unsigned(u64),
    Float(f64),
    Text(String),
    Bytes(Vec<u8>),
    Array(Vec<Value>),
    Object(Object),
}

impl PartialEq for Value {
    fn eq(&self, other: &Self) -> bool {
        if let (Some(left), Some(right)) = (self.array(), other.array()) {
            return left == right;
        }
        if let (Some(left), Some(right)) = (self.object(), other.object()) {
            return left == right;
        }
        match (self, other) {
            (Self::Null, Self::Null) => true,
            (Self::Bool(left), Self::Bool(right)) => left == right,
            (Self::Signed(left), Self::Signed(right)) => left == right,
            (Self::Unsigned(left), Self::Unsigned(right)) => left == right,
            (Self::Float(left), Self::Float(right)) => left == right,
            (Self::Text(left), Self::Text(right)) => left == right,
            (Self::Bytes(left), Self::Bytes(right)) => left == right,
            _ => false,
        }
    }
}

macro_rules! borrowed_value_accessor {
    ($name:ident, $variant:ident, $output:ty) => {
        pub fn $name(&self) -> Option<&$output> {
            match self {
                Self::$variant(value) => Some(value),
                _ => None,
            }
        }
    };
}

macro_rules! copied_value_accessor {
    ($name:ident, $variant:ident, $output:ty) => {
        pub fn $name(&self) -> Option<$output> {
            match self {
                Self::$variant(value) => Some(*value),
                _ => None,
            }
        }
    };
}

impl Value {
    pub fn object(&self) -> Option<&[(String, Value)]> {
        match self {
            Self::Object(value) => Some(value),
            _ => None,
        }
    }

    pub fn object_mut(&mut self) -> Option<&mut [(String, Value)]> {
        match self {
            Self::Object(value) => Some(value.as_mut_slice()),
            _ => None,
        }
    }

    pub fn array(&self) -> Option<&[Value]> {
        match self {
            Self::Array(value) => Some(value),
            _ => None,
        }
    }

    pub fn array_mut(&mut self) -> Option<&mut [Value]> {
        match self {
            Self::Array(value) => Some(value.as_mut_slice()),
            _ => None,
        }
    }

    borrowed_value_accessor!(text, Text, str);
    borrowed_value_accessor!(bytes, Bytes, [u8]);
    copied_value_accessor!(unsigned, Unsigned, u64);
    copied_value_accessor!(signed, Signed, i64);
    copied_value_accessor!(boolean, Bool, bool);

    pub fn integer_i64(&self) -> Option<i64> {
        self.signed()
            .or_else(|| self.unsigned().and_then(|value| i64::try_from(value).ok()))
    }

    pub fn integer_u64(&self) -> Option<u64> {
        self.unsigned()
            .or_else(|| self.signed().and_then(|value| u64::try_from(value).ok()))
    }

    pub fn number(&self) -> Option<f64> {
        match self {
            Self::Float(value) => Some(*value),
            Self::Signed(value) => Some(*value as f64),
            Self::Unsigned(value) => Some(*value as f64),
            _ => None,
        }
    }

    pub fn field(&self, name: &str) -> Option<&Value> {
        self.object()?
            .iter()
            .find_map(|(key, value)| (key == name).then_some(value))
    }

    pub fn field_mut(&mut self, name: &str) -> Option<&mut Value> {
        self.object_mut()?
            .iter_mut()
            .find_map(|(key, value)| (key == name).then_some(value))
    }
}

pub trait IntoApplicationValue {
    fn into_application_value(self) -> Value;
}

pub trait FromApplicationValue: Sized {
    fn from_application_value(value: Value) -> Result<Self, String>;
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ByteArray<const N: usize>(pub [u8; N]);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ByteBuffer(pub Vec<u8>);

pub(crate) fn object(value: Value) -> Result<Vec<(String, Value)>, String> {
    let Value::Object(fields) = value else {
        return Err("object expected".into());
    };
    for (index, (name, _)) in fields.iter().enumerate() {
        if fields[..index].iter().any(|(prior, _)| prior == name) {
            return Err(format!("duplicate field {name}"));
        }
    }
    Ok(fields)
}

pub(crate) fn take_field(fields: &mut Vec<(String, Value)>, name: &str) -> Result<Value, String> {
    let index = fields
        .iter()
        .position(|(candidate, _)| candidate == name)
        .ok_or_else(|| format!("missing field {name}"))?;
    Ok(fields.swap_remove(index).1)
}

pub(crate) fn take_optional_field(fields: &mut Vec<(String, Value)>, name: &str) -> Value {
    fields
        .iter()
        .position(|(candidate, _)| candidate == name)
        .map_or(Value::Null, |index| fields.swap_remove(index).1)
}

pub(crate) fn application_value_within_limits(
    value: &Value,
    max_bytes: usize,
    max_items: usize,
    max_depth: usize,
    depth: usize,
) -> bool {
    if depth > max_depth {
        return false;
    }
    match value {
        Value::Text(value) => max_bytes == 0 || value.len() <= max_bytes,
        Value::Bytes(value) => max_bytes == 0 || value.len() <= max_bytes,
        Value::Array(values) => {
            (max_items == 0 || values.len() <= max_items)
                && values.iter().all(|value| {
                    application_value_within_limits(
                        value,
                        max_bytes,
                        max_items,
                        max_depth,
                        depth + 1,
                    )
                })
        }
        Value::Object(values) => {
            (max_items == 0 || values.len() <= max_items)
                && values.iter().all(|(name, value)| {
                    (max_bytes == 0 || name.len() <= max_bytes)
                        && application_value_within_limits(
                            value,
                            max_bytes,
                            max_items,
                            max_depth,
                            depth + 1,
                        )
                })
        }
        Value::Float(value) => value.is_finite(),
        _ => true,
    }
}

impl IntoApplicationValue for Value {
    fn into_application_value(self) -> Value {
        self
    }
}

impl FromApplicationValue for Value {
    fn from_application_value(value: Value) -> Result<Self, String> {
        Ok(value)
    }
}

impl IntoApplicationValue for () {
    fn into_application_value(self) -> Value {
        Value::Object(Vec::new())
    }
}

impl FromApplicationValue for () {
    fn from_application_value(value: Value) -> Result<Self, String> {
        if matches!(value, Value::Object(ref fields) if fields.is_empty()) {
            Ok(())
        } else {
            Err("empty object expected".into())
        }
    }
}

impl IntoApplicationValue for bool {
    fn into_application_value(self) -> Value {
        Value::Bool(self)
    }
}

impl FromApplicationValue for bool {
    fn from_application_value(value: Value) -> Result<Self, String> {
        if let Value::Bool(value) = value {
            Ok(value)
        } else {
            Err("bool expected".into())
        }
    }
}

macro_rules! unsigned_value {
    ($($type:ty),*) => {
        $(
            impl IntoApplicationValue for $type {
                fn into_application_value(self) -> Value {
                    Value::Unsigned(self as u64)
                }
            }

            impl FromApplicationValue for $type {
                fn from_application_value(value: Value) -> Result<Self, String> {
                    let value = value
                        .integer_u64()
                        .ok_or_else(|| "unsigned integer expected".to_owned())?;
                    <$type>::try_from(value).map_err(|_| "unsigned integer overflow".into())
                }
            }
        )*
    };
}
unsigned_value!(u8, u16, u32, u64, usize);

macro_rules! signed_value {
    ($($type:ty),*) => {
        $(
            impl IntoApplicationValue for $type {
                fn into_application_value(self) -> Value {
                    Value::Signed(self as i64)
                }
            }

            impl FromApplicationValue for $type {
                fn from_application_value(value: Value) -> Result<Self, String> {
                    let value = value
                        .integer_i64()
                        .ok_or_else(|| "signed integer expected".to_owned())?;
                    <$type>::try_from(value).map_err(|_| "signed integer overflow".into())
                }
            }
        )*
    };
}
signed_value!(i8, i16, i32, i64, isize);

impl IntoApplicationValue for f32 {
    fn into_application_value(self) -> Value {
        Value::Float(self as f64)
    }
}

impl FromApplicationValue for f32 {
    fn from_application_value(value: Value) -> Result<Self, String> {
        let Value::Float(value) = value else {
            return Err("float expected".into());
        };
        let converted = value as f32;
        if !converted.is_finite() && value.is_finite() {
            Err("f32 overflow".into())
        } else {
            Ok(converted)
        }
    }
}

impl IntoApplicationValue for f64 {
    fn into_application_value(self) -> Value {
        Value::Float(self)
    }
}

impl FromApplicationValue for f64 {
    fn from_application_value(value: Value) -> Result<Self, String> {
        if let Value::Float(value) = value {
            Ok(value)
        } else {
            Err("float expected".into())
        }
    }
}

impl IntoApplicationValue for String {
    fn into_application_value(self) -> Value {
        Value::Text(self)
    }
}

impl FromApplicationValue for String {
    fn from_application_value(value: Value) -> Result<Self, String> {
        if let Value::Text(value) = value {
            Ok(value)
        } else {
            Err("text expected".into())
        }
    }
}

impl IntoApplicationValue for Cow<'static, str> {
    fn into_application_value(self) -> Value {
        Value::Text(self.into_owned())
    }
}

impl FromApplicationValue for Cow<'static, str> {
    fn from_application_value(value: Value) -> Result<Self, String> {
        if let Value::Text(value) = value {
            Ok(Cow::Owned(value))
        } else {
            Err("text expected".into())
        }
    }
}

impl<T: IntoApplicationValue> IntoApplicationValue for Vec<T> {
    fn into_application_value(self) -> Value {
        Value::Array(
            self.into_iter()
                .map(IntoApplicationValue::into_application_value)
                .collect(),
        )
    }
}

impl<T: FromApplicationValue> FromApplicationValue for Vec<T> {
    fn from_application_value(value: Value) -> Result<Self, String> {
        let Value::Array(values) = value else {
            return Err("array expected".into());
        };
        values
            .into_iter()
            .map(FromApplicationValue::from_application_value)
            .collect()
    }
}

impl<T: IntoApplicationValue, const N: usize> IntoApplicationValue for [T; N] {
    fn into_application_value(self) -> Value {
        Value::Array(
            self.into_iter()
                .map(IntoApplicationValue::into_application_value)
                .collect(),
        )
    }
}

impl<T: FromApplicationValue, const N: usize> FromApplicationValue for [T; N] {
    fn from_application_value(value: Value) -> Result<Self, String> {
        let values: Vec<T> = FromApplicationValue::from_application_value(value)?;
        values
            .try_into()
            .map_err(|_| format!("array must contain {N} items"))
    }
}

impl<const N: usize> IntoApplicationValue for ByteArray<N> {
    fn into_application_value(self) -> Value {
        Value::Bytes(self.0.to_vec())
    }
}

impl<const N: usize> FromApplicationValue for ByteArray<N> {
    fn from_application_value(value: Value) -> Result<Self, String> {
        let Value::Bytes(bytes) = value else {
            return Err("bytes expected".into());
        };
        Ok(Self(bytes.try_into().map_err(|_| {
            format!("byte array must contain {N} items")
        })?))
    }
}

impl IntoApplicationValue for ByteBuffer {
    fn into_application_value(self) -> Value {
        Value::Bytes(self.0)
    }
}

impl FromApplicationValue for ByteBuffer {
    fn from_application_value(value: Value) -> Result<Self, String> {
        if let Value::Bytes(bytes) = value {
            Ok(Self(bytes))
        } else {
            Err("bytes expected".into())
        }
    }
}

impl<T: IntoApplicationValue> IntoApplicationValue for Option<T> {
    fn into_application_value(self) -> Value {
        self.map_or(Value::Null, IntoApplicationValue::into_application_value)
    }
}

impl<T: FromApplicationValue> FromApplicationValue for Option<T> {
    fn from_application_value(value: Value) -> Result<Self, String> {
        if matches!(value, Value::Null) {
            Ok(None)
        } else {
            T::from_application_value(value).map(Some)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn object_extraction_rejects_duplicate_and_missing_fields() {
        assert!(
            object(Value::Object(vec![
                ("value".into(), Value::Null),
                ("value".into(), Value::Null),
            ]))
            .is_err()
        );
        let mut fields =
            object(Value::Object(vec![("value".into(), Value::Bool(true))])).expect("object");
        assert_eq!(take_field(&mut fields, "value"), Ok(Value::Bool(true)));
        assert!(take_field(&mut fields, "missing").is_err());
    }

    #[test]
    fn primitive_collection_and_optional_codecs_round_trip() {
        let value = vec![Some(7_u16), None].into_application_value();
        assert_eq!(
            Vec::<Option<u16>>::from_application_value(value),
            Ok(vec![Some(7), None])
        );
        assert!(u8::from_application_value(Value::Unsigned(256)).is_err());
        assert!(f32::from_application_value(Value::Float(f64::MAX)).is_err());
    }

    #[test]
    fn bounded_values_reject_depth_size_and_non_finite_floats() {
        assert!(application_value_within_limits(
            &Value::Array(vec![Value::Text("ok".into())]),
            2,
            1,
            1,
            0,
        ));
        assert!(!application_value_within_limits(
            &Value::Text("long".into()),
            2,
            0,
            1,
            0,
        ));
        assert!(!application_value_within_limits(
            &Value::Float(f64::NAN),
            0,
            0,
            1,
            0,
        ));
        assert!(!application_value_within_limits(&Value::Null, 0, 0, 1, 2,));
    }
}
