// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

//! [`VarULE`] impls for tuples.
//!
//! This module exports [`Tuple2VarULE`], [`Tuple3VarULE`], ..., the corresponding [`VarULE`] types
//! of tuples containing purely [`VarULE`] types.
//!
//! This can be paired with [`VarTupleULE`] to make arbitrary combinations of [`ULE`] and [`VarULE`] types.
//!
//! [`VarTupleULE`]: crate::ule::vartuple::VarTupleULE

use super::*;
use crate::varzerovec::{Index16, VarZeroVecFormat};
use core::fmt;
use core::marker::PhantomData;
use core::mem;
use zerofrom::ZeroFrom;

macro_rules! tuple_varule {
    ($name:ident, $len:literal, [ $($T:ident $t:ident $T_alt: ident $i:tt),+ ]) => {
        #[doc = concat!("VarULE type for tuples with ", $len, " elements. See module docs for more information")]
        #[repr(transparent)]
        #[allow(clippy::exhaustive_structs)] 
        pub struct $name<$($T: ?Sized,)+ Format: VarZeroVecFormat = Index16> {
            $($t: PhantomData<$T>,)+
            multi: MultiFieldsULE<$len, Format>
        }

        impl<$($T: VarULE + ?Sized,)+ Format: VarZeroVecFormat> $name<$($T,)+ Format> {
            $(
                #[doc = concat!("Get field ", $i, "of this tuple")]
                pub fn $t(&self) -> &$T {
                    unsafe {
                        self.multi.get_field::<$T>($i)
                    }
                }


            )+
        }

        unsafe impl<$($T: VarULE + ?Sized,)+ Format: VarZeroVecFormat> VarULE for $name<$($T,)+ Format>
        {
            fn validate_bytes(bytes: &[u8]) -> Result<(), UleError> {
                let multi = <MultiFieldsULE<$len, Format> as VarULE>::parse_bytes(bytes)?;
                $(
                    unsafe {
                        multi.validate_field::<$T>($i)?;
                    }
                )+
                Ok(())
            }

            unsafe fn from_bytes_unchecked(bytes: &[u8]) -> &Self {
                let multi = <MultiFieldsULE<$len, Format> as VarULE>::from_bytes_unchecked(bytes);

                mem::transmute::<&MultiFieldsULE<$len, Format>, &Self>(multi)
            }
        }

        impl<$($T: fmt::Debug + VarULE + ?Sized,)+ Format: VarZeroVecFormat> fmt::Debug for $name<$($T,)+ Format> {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> Result<(), fmt::Error> {
                ($(self.$t(),)+).fmt(f)
            }
        }

        impl<$($T: PartialEq + VarULE + ?Sized,)+ Format: VarZeroVecFormat> PartialEq for $name<$($T,)+ Format> {
            fn eq(&self, other: &Self) -> bool {

                ($(self.$t(),)+).eq(&($(other.$t(),)+))
            }
        }

        impl<$($T: Eq + VarULE + ?Sized,)+ Format: VarZeroVecFormat> Eq for $name<$($T,)+ Format> {}

        impl<$($T: PartialOrd + VarULE + ?Sized,)+ Format: VarZeroVecFormat> PartialOrd for $name<$($T,)+ Format> {
            fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
                ($(self.$t(),)+).partial_cmp(&($(other.$t(),)+))
            }
        }

        impl<$($T: Ord + VarULE + ?Sized,)+ Format: VarZeroVecFormat> Ord for $name<$($T,)+ Format>  {
            fn cmp(&self, other: &Self) -> core::cmp::Ordering {
                ($(self.$t(),)+).cmp(&($(other.$t(),)+))
            }
        }

        unsafe impl<$($T,)+ $($T_alt,)+ Format> EncodeAsVarULE<$name<$($T,)+ Format>> for ( $($T_alt),+ )
        where
            $($T: VarULE + ?Sized,)+
            $($T_alt: EncodeAsVarULE<$T>,)+
            Format: VarZeroVecFormat,
        {
            fn encode_var_ule_as_slices<R>(&self, _: impl FnOnce(&[&[u8]]) -> R) -> R {
                unreachable!()
            }

            #[inline]
            fn encode_var_ule_len(&self) -> usize {
                MultiFieldsULE::<$len, Format>::compute_encoded_len_for([$(self.$i.encode_var_ule_len()),+])
            }

            #[inline]
            fn encode_var_ule_write(&self, dst: &mut [u8]) {
                let lengths = [$(self.$i.encode_var_ule_len()),+];
                let multi = MultiFieldsULE::<$len, Format>::new_from_lengths_partially_initialized(lengths, dst);
                $(
                    unsafe {
                        multi.set_field_at::<$T, $T_alt>($i, &self.$i);
                    }
                )+
            }
        }

        #[cfg(feature = "alloc")]
        impl<$($T: VarULE + ?Sized,)+ Format: VarZeroVecFormat> alloc::borrow::ToOwned for $name<$($T,)+ Format> {
            type Owned = alloc::boxed::Box<Self>;
            fn to_owned(&self) -> Self::Owned {
                encode_varule_to_box(self)
            }
        }

        impl<'a, $($T,)+ $($T_alt,)+ Format> ZeroFrom <'a, $name<$($T,)+ Format>> for ($($T_alt),+)
        where
                    $($T: VarULE + ?Sized,)+
                    $($T_alt: ZeroFrom<'a, $T>,)+
                    Format: VarZeroVecFormat {
            fn zero_from(other: &'a $name<$($T,)+ Format>) -> Self {
                (
                    $($T_alt::zero_from(other.$t()),)+
                )
            }
        }

        #[cfg(feature = "serde")]
        impl<$($T: serde::Serialize,)+ Format> serde::Serialize for $name<$($T,)+ Format>
        where
            $($T: VarULE + ?Sized,)+
            $(for<'a> &'a $T: ZeroFrom<'a, $T>,)+
            Format: VarZeroVecFormat
        {
            fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error> where S: serde::Serializer {
                if serializer.is_human_readable() {
                    let this = (
                        $(self.$t()),+
                    );
                    <($(&$T),+) as serde::Serialize>::serialize(&this, serializer)
                } else {
                    serializer.serialize_bytes(self.multi.as_bytes())
                }
            }
        }

        #[cfg(feature = "serde")]
        impl<'de, $($T: VarULE + ?Sized,)+ Format> serde::Deserialize<'de> for alloc::boxed::Box<$name<$($T,)+ Format>>
            where
                $( alloc::boxed::Box<$T>: serde::Deserialize<'de>,)+
                Format: VarZeroVecFormat {
            fn deserialize<Des>(deserializer: Des) -> Result<Self, Des::Error> where Des: serde::Deserializer<'de> {
                if deserializer.is_human_readable() {
                    let this = <( $(alloc::boxed::Box<$T>),+) as serde::Deserialize>::deserialize(deserializer)?;
                    let this_ref = (
                        $(&*this.$i),+
                    );
                    Ok(crate::ule::encode_varule_to_box(&this_ref))
                } else {

                    let deserialized = <&$name<$($T,)+ Format>>::deserialize(deserializer)?;
                    Ok(deserialized.to_boxed())
                }
            }
        }

        #[cfg(feature = "serde")]
        impl<'a, 'de: 'a, $($T: VarULE + ?Sized,)+ Format: VarZeroVecFormat> serde::Deserialize<'de> for &'a $name<$($T,)+ Format> {
            fn deserialize<Des>(deserializer: Des) -> Result<Self, Des::Error> where Des: serde::Deserializer<'de> {
                if deserializer.is_human_readable() {
                    Err(serde::de::Error::custom(
                        concat!("&", stringify!($name), " can only deserialize in zero-copy ways"),
                    ))
                } else {
                    let bytes = <&[u8]>::deserialize(deserializer)?;
                    $name::<$($T,)+ Format>::parse_bytes(bytes).map_err(serde::de::Error::custom)
                }
            }
        }
    };
}

tuple_varule!(Tuple2VarULE, 2, [ A a AE 0, B b BE 1 ]);
tuple_varule!(Tuple3VarULE, 3, [ A a AE 0, B b BE 1, C c CE 2 ]);
tuple_varule!(Tuple4VarULE, 4, [ A a AE 0, B b BE 1, C c CE 2, D d DE 3 ]);
tuple_varule!(Tuple5VarULE, 5, [ A a AE 0, B b BE 1, C c CE 2, D d DE 3, E e EE 4 ]);
tuple_varule!(Tuple6VarULE, 6, [ A a AE 0, B b BE 1, C c CE 2, D d DE 3, E e EE 4, F f FE 5 ]);
