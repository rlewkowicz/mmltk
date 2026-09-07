// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

//! Types to help compose fixed-size [`ULE`] and variable-size [`VarULE`] primitives.
//!
//! This module exports [`VarTuple`] and [`VarTupleULE`], which allow a single sized type and
//! a single unsized type to be stored together as a [`VarULE`].
//!
//! # Examples
//!
//! ```
//! use zerovec::ule::vartuple::{VarTuple, VarTupleULE};
//! use zerovec::VarZeroVec;
//!
//! struct Employee<'a> {
//!     id: u32,
//!     name: &'a str,
//! };
//!
//! let employees = [
//!     Employee {
//!         id: 12345,
//!         name: "Jane Doe",
//!     },
//!     Employee {
//!         id: 67890,
//!         name: "John Doe",
//!     },
//! ];
//!
//! let employees_as_var_tuples = employees
//!     .into_iter()
//!     .map(|x| VarTuple {
//!         sized: x.id,
//!         variable: x.name,
//!     })
//!     .collect::<Vec<_>>();
//!
//! let employees_vzv: VarZeroVec<VarTupleULE<u32, str>> =
//!     employees_as_var_tuples.as_slice().into();
//!
//! assert_eq!(employees_vzv.len(), 2);
//!
//! assert_eq!(employees_vzv.get(0).unwrap().sized.as_unsigned_int(), 12345);
//! assert_eq!(&employees_vzv.get(0).unwrap().variable, "Jane Doe");
//!
//! assert_eq!(employees_vzv.get(1).unwrap().sized.as_unsigned_int(), 67890);
//! assert_eq!(&employees_vzv.get(1).unwrap().variable, "John Doe");
//! ```

use core::mem::{size_of, transmute_copy};
use zerofrom::ZeroFrom;

use super::{AsULE, EncodeAsVarULE, UleError, VarULE, ULE};

/// A sized type that can be converted to a [`VarTupleULE`].
///
/// See the module for examples.
#[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Clone)]
#[allow(clippy::exhaustive_structs)] 
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct VarTuple<A, B> {
    pub sized: A,
    pub variable: B,
}

/// A dynamically-sized type combining a sized and an unsized type.
///
/// See the module for examples.
#[derive(Debug, PartialEq, Eq, PartialOrd, Ord)]
#[allow(clippy::exhaustive_structs)] 
#[repr(C)]
pub struct VarTupleULE<A: AsULE, V: VarULE + ?Sized> {
    pub sized: A::ULE,
    pub variable: V,
}

unsafe impl<A, V> VarULE for VarTupleULE<A, V>
where
    A: AsULE + 'static,
    V: VarULE + ?Sized,
{
    fn validate_bytes(bytes: &[u8]) -> Result<(), UleError> {
        let (sized_chunk, variable_chunk) = bytes
            .split_at_checked(size_of::<A::ULE>())
            .ok_or(UleError::length::<Self>(bytes.len()))?;
        A::ULE::validate_bytes(sized_chunk)?;
        V::validate_bytes(variable_chunk)?;
        Ok(())
    }

    unsafe fn from_bytes_unchecked(bytes: &[u8]) -> &Self {
        let (_sized_chunk, variable_chunk) = bytes.split_at_unchecked(size_of::<A::ULE>());
        let variable_ref = V::from_bytes_unchecked(variable_chunk);
        let variable_ptr: *const V = variable_ref;



        assert_eq!(size_of::<*const V>(), size_of::<(*const u8, usize)>());
        let (_v_ptr, metadata) = transmute_copy::<*const V, (*const u8, usize)>(&variable_ptr);

        assert_eq!(size_of::<*const Self>(), size_of::<(*const u8, usize)>());
        let composed_ptr =
            transmute_copy::<(*const u8, usize), *const Self>(&(bytes.as_ptr(), metadata));
        &*(composed_ptr)
    }
}

unsafe impl<A, B, V> EncodeAsVarULE<VarTupleULE<A, V>> for VarTuple<A, B>
where
    A: AsULE + 'static,
    B: EncodeAsVarULE<V>,
    V: VarULE + ?Sized,
{
    fn encode_var_ule_as_slices<R>(&self, _: impl FnOnce(&[&[u8]]) -> R) -> R {
        unreachable!()
    }

    #[inline]
    fn encode_var_ule_len(&self) -> usize {
        size_of::<A::ULE>() + self.variable.encode_var_ule_len()
    }

    #[inline]
    fn encode_var_ule_write(&self, dst: &mut [u8]) {
        let (sized_chunk, variable_chunk) = dst.split_at_mut(size_of::<A::ULE>());
        sized_chunk.clone_from_slice([self.sized.to_unaligned()].as_bytes());
        self.variable.encode_var_ule_write(variable_chunk);
    }
}

#[cfg(feature = "alloc")]
impl<A, V> alloc::borrow::ToOwned for VarTupleULE<A, V>
where
    A: AsULE + 'static,
    V: VarULE + ?Sized,
{
    type Owned = alloc::boxed::Box<Self>;
    fn to_owned(&self) -> Self::Owned {
        crate::ule::encode_varule_to_box(self)
    }
}

impl<'a, A, B, V> ZeroFrom<'a, VarTupleULE<A, V>> for VarTuple<A, B>
where
    A: AsULE + 'static,
    V: VarULE + ?Sized,
    B: ZeroFrom<'a, V>,
{
    fn zero_from(other: &'a VarTupleULE<A, V>) -> Self {
        VarTuple {
            sized: AsULE::from_unaligned(other.sized),
            variable: B::zero_from(&other.variable),
        }
    }
}

#[cfg(feature = "serde")]
impl<A, V> serde::Serialize for VarTupleULE<A, V>
where
    A: AsULE + 'static,
    V: VarULE + ?Sized,
    A: serde::Serialize,
    V: serde::Serialize,
{
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        if serializer.is_human_readable() {
            let this = VarTuple {
                sized: A::from_unaligned(self.sized),
                variable: &self.variable,
            };
            this.serialize(serializer)
        } else {
            serializer.serialize_bytes(self.as_bytes())
        }
    }
}

#[cfg(feature = "serde")]
impl<'a, 'de: 'a, A, V> serde::Deserialize<'de> for &'a VarTupleULE<A, V>
where
    A: AsULE + 'static,
    V: VarULE + ?Sized,
    A: serde::Deserialize<'de>,
{
    fn deserialize<Des>(deserializer: Des) -> Result<Self, Des::Error>
    where
        Des: serde::Deserializer<'de>,
    {
        if !deserializer.is_human_readable() {
            let bytes = <&[u8]>::deserialize(deserializer)?;
            VarTupleULE::<A, V>::parse_bytes(bytes).map_err(serde::de::Error::custom)
        } else {
            Err(serde::de::Error::custom(
                "&VarTupleULE can only deserialize in zero-copy ways",
            ))
        }
    }
}

#[cfg(feature = "serde")]
impl<'de, A, V> serde::Deserialize<'de> for alloc::boxed::Box<VarTupleULE<A, V>>
where
    A: AsULE + 'static,
    V: VarULE + ?Sized,
    A: serde::Deserialize<'de>,
    alloc::boxed::Box<V>: serde::Deserialize<'de>,
{
    fn deserialize<Des>(deserializer: Des) -> Result<Self, Des::Error>
    where
        Des: serde::Deserializer<'de>,
    {
        if deserializer.is_human_readable() {
            let this = VarTuple::<A, alloc::boxed::Box<V>>::deserialize(deserializer)?;
            Ok(crate::ule::encode_varule_to_box(&this))
        } else {

            let deserialized = <&VarTupleULE<A, V>>::deserialize(deserializer)?;
            Ok(deserialized.to_boxed())
        }
    }
}
