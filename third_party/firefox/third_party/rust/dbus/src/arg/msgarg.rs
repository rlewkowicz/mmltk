#![allow(dead_code)]

use {Signature, Message, arg::TypeMismatchError};
use std::{fmt, any};
use std::sync::Arc;
use std::rc::Rc;

use super::{Iter, IterAppend, ArgType};

/// Types that can represent a D-Bus message argument implement this trait.
///
/// Types should also implement either Append or Get to be useful. 
pub trait Arg {
    /// The corresponding D-Bus argument type code. 
    const ARG_TYPE: ArgType;
    /// The corresponding D-Bus argument type code; just returns ARG_TYPE. 
    ///
    /// For backwards compatibility.
    #[deprecated(note = "Use associated constant ARG_TYPE instead")]
    fn arg_type() -> ArgType { return Self::ARG_TYPE; }
    /// The corresponding D-Bus type signature for this type. 
    fn signature() -> Signature<'static>;
}

/// Types that can be appended to a message as arguments implement this trait.
pub trait Append: Sized {
    /// Performs the append operation.
    fn append(self, &mut IterAppend);
}

/// Helper trait to append many arguments to a message.
pub trait AppendAll: Sized {
    /// Performs the append operation.
    fn append(self, &mut IterAppend);
}

/// Types that can be retrieved from a message as arguments implement this trait.
pub trait Get<'a>: Sized {
    /// Performs the get operation.
    fn get(i: &mut Iter<'a>) -> Option<Self>;
}

/// Helper trait to read all arguments from a message.
pub trait ReadAll: Sized {
    /// Performs the read operation.
    fn read(i: &mut Iter) -> Result<Self, TypeMismatchError>;
}


/// Object safe version of Arg + Append + Get.
pub trait RefArg: fmt::Debug {
    /// The corresponding D-Bus argument type code.
    fn arg_type(&self) -> ArgType;
    /// The corresponding D-Bus type signature for this type. 
    fn signature(&self) -> Signature<'static>;
    /// Performs the append operation.
    fn append(&self, &mut IterAppend);
    /// Transforms this argument to Any (which can be downcasted to read the current value).
    ///
    /// Note: The internal representation of complex types (Array, Dict, Struct) is unstable
    /// and as_any should not be relied upon for these types. Use as_iter instead.
    fn as_any(&self) -> &any::Any where Self: 'static;
    /// Transforms this argument to Any (which can be downcasted to read the current value).
    ///
    /// Note: The internal representation of complex types (Array, Dict, Struct) is unstable
    /// and as_any should not be relied upon for these types. Use as_iter instead.
    ///
    /// # Panic
    /// Will panic if the interior cannot be made mutable, e g, if encapsulated
    /// inside a Rc with a reference count > 1.
    fn as_any_mut(&mut self) -> &mut any::Any where Self: 'static;
    /// Try to read the argument as an i64.
    ///
    /// Works for: Boolean, Byte, Int16, UInt16, Int32, UInt32, Int64, UnixFd.
    #[inline]
    fn as_i64(&self) -> Option<i64> { None }
    /// Try to read the argument as an u64.
    ///
    /// Works for: Boolean, Byte, Int16, UInt16, Int32, UInt32, UInt64.
    #[inline]
    fn as_u64(&self) -> Option<u64> { None }
    /// Try to read the argument as an f64.
    ///
    /// Works for: Boolean, Byte, Int16, UInt16, Int32, UInt32, Double.
    #[inline]
    fn as_f64(&self) -> Option<f64> { None }
    /// Try to read the argument as a str.
    ///
    /// Works for: String, ObjectPath, Signature.
    #[inline]
    fn as_str(&self) -> Option<&str> { None }
    /// Try to read the argument as an iterator.
    ///
    /// Works for: Array/Dict, Struct, Variant.
    #[inline]
    fn as_iter<'a>(&'a self) -> Option<Box<Iterator<Item=&'a RefArg> + 'a>> { None }
    /// Deep clone of the RefArg, causing the result to be 'static.
    ///
    /// Usable as an escape hatch in case of lifetime problems with RefArg.
    ///
    /// In case of complex types (Array, Dict, Struct), the clone is not guaranteed
    /// to have the same internal representation as the original.
    fn box_clone(&self) -> Box<RefArg + 'static> { unimplemented!()  }
}

impl<'a> Get<'a> for Box<RefArg> {
    fn get(i: &mut Iter<'a>) -> Option<Self> { i.get_refarg() }
}

/// Cast a RefArg as a specific type (shortcut for any + downcast)
#[inline]
pub fn cast<'a, T: 'static>(a: &'a (RefArg + 'static)) -> Option<&'a T> { a.as_any().downcast_ref() }

/// Cast a RefArg as a specific type (shortcut for any_mut + downcast_mut)
///
/// # Panic
/// Will panic if the interior cannot be made mutable, e g, if encapsulated
/// inside a Rc with a reference count > 1.
#[inline]
pub fn cast_mut<'a, T: 'static>(a: &'a mut (RefArg + 'static)) -> Option<&'a mut T> { a.as_any_mut().downcast_mut() }

/// If a type implements this trait, it means the size and alignment is the same
/// as in D-Bus. This means that you can quickly append and get slices of this type.
///
/// Note: Booleans do not implement this trait because D-Bus booleans are 4 bytes and Rust booleans are 1 byte.
pub unsafe trait FixedArray: Arg + 'static + Clone + Copy {}

/// Types that can be used as keys in a dict type implement this trait. 
pub trait DictKey: Arg {}



/// Simple lift over reference to value - this makes some iterators more ergonomic to use
impl<'a, T: Arg> Arg for &'a T {
    const ARG_TYPE: ArgType = T::ARG_TYPE;
    fn signature() -> Signature<'static> { T::signature() }
}
impl<'a, T: Append + Clone> Append for &'a T {
    fn append(self, i: &mut IterAppend) { self.clone().append(i) }
}
impl<'a, T: DictKey> DictKey for &'a T {}

impl<'a, T: RefArg + ?Sized> RefArg for &'a T {
    #[inline]
    fn arg_type(&self) -> ArgType { (&**self).arg_type() }
    #[inline]
    fn signature(&self) -> Signature<'static> { (&**self).signature() }
    #[inline]
    fn append(&self, i: &mut IterAppend) { (&**self).append(i) }
    #[inline]
    fn as_any(&self) -> &any::Any where T: 'static { (&**self).as_any() }
    #[inline]
    fn as_any_mut(&mut self) -> &mut any::Any where T: 'static { unreachable!() }
    #[inline]
    fn as_i64(&self) -> Option<i64> { (&**self).as_i64() }
    #[inline]
    fn as_u64(&self) -> Option<u64> { (&**self).as_u64() }
    #[inline]
    fn as_f64(&self) -> Option<f64> { (&**self).as_f64() }
    #[inline]
    fn as_str(&self) -> Option<&str> { (&**self).as_str() }
    #[inline]
    fn as_iter<'b>(&'b self) -> Option<Box<Iterator<Item=&'b RefArg> + 'b>> { (&**self).as_iter() }
    #[inline]
    fn box_clone(&self) -> Box<RefArg + 'static> { (&**self).box_clone() }
}



macro_rules! deref_impl {
    ($t: ident, $ss: ident, $make_mut: expr) => {

impl<T: RefArg + ?Sized> RefArg for $t<T> {
    #[inline]
    fn arg_type(&self) -> ArgType { (&**self).arg_type() }
    #[inline]
    fn signature(&self) -> Signature<'static> { (&**self).signature() }
    #[inline]
    fn append(&self, i: &mut IterAppend) { (&**self).append(i) }
    #[inline]
    fn as_any(&self) -> &any::Any where T: 'static { (&**self).as_any() }
    #[inline]
    fn as_any_mut<'a>(&'a mut $ss) -> &'a mut any::Any where T: 'static { $make_mut.as_any_mut() }
    #[inline]
    fn as_i64(&self) -> Option<i64> { (&**self).as_i64() }
    #[inline]
    fn as_u64(&self) -> Option<u64> { (&**self).as_u64() }
    #[inline]
    fn as_f64(&self) -> Option<f64> { (&**self).as_f64() }
    #[inline]
    fn as_str(&self) -> Option<&str> { (&**self).as_str() }
    #[inline]
    fn as_iter<'a>(&'a self) -> Option<Box<Iterator<Item=&'a RefArg> + 'a>> { (&**self).as_iter() }
    #[inline]
    fn box_clone(&self) -> Box<RefArg + 'static> { (&**self).box_clone() }
}
impl<T: DictKey> DictKey for $t<T> {}

impl<T: Arg> Arg for $t<T> {
    const ARG_TYPE: ArgType = T::ARG_TYPE;
    fn signature() -> Signature<'static> { T::signature() }
}
impl<'a, T: Get<'a>> Get<'a> for $t<T> {
    fn get(i: &mut Iter<'a>) -> Option<Self> { T::get(i).map(|v| $t::new(v)) }
}

    }
}

impl<T: Append> Append for Box<T> {
    fn append(self, i: &mut IterAppend) { let q: T = *self; q.append(i) }
}

deref_impl!(Box, self, &mut **self );
deref_impl!(Rc, self, Rc::get_mut(self).unwrap());
deref_impl!(Arc, self, Arc::get_mut(self).unwrap());

/// Internal trait to help generics. Implemented for (), (A1), (A1, A2) and so on (where A1: Arg, A2: Arg etc).
///
/// You would probably not use this trait directly, instead use generic functions which
/// take ArgBuilder as an argument. It helps reading and appending multiple arguments
/// to/from a message in one go.
pub trait ArgBuilder: Sized {
    /// A tuple of &static str. Used for introspection.
    type strs;
    /// Low-level introspection helper method.
    fn strs_sig<F: FnMut(&'static str, Signature<'static>)>(a: Self::strs, f: F);
    /// Low-level method to read arguments from a message.
    fn read(msg: &Message) -> Result<Self, TypeMismatchError>;
    /// Low-level method to append arguments to a message.
    fn append(self, msg: &mut Message);
}

impl ArgBuilder for () {
    type strs = ();
    fn strs_sig<F: FnMut(&'static str, Signature<'static>)>(_: Self::strs, _: F) {}
    fn read(_: &Message) -> Result<Self, TypeMismatchError> { Ok(()) }
    fn append(self, _: &mut Message) {}
}

macro_rules! argbuilder_impl {
    ($($n: ident $t: ident $s: ty,)+) => {

impl<$($t: Arg + Append + for<'z> Get<'z>),*> ArgBuilder for ($($t,)*) {
    type strs = ($(&'static $s,)*); 
    fn strs_sig<Q: FnMut(&'static str, Signature<'static>)>(z: Self::strs, mut q: Q) {
        let ( $($n,)*) = z;
        $( q($n, $t::signature()); )*
    }

    fn read(msg: &Message) -> Result<Self, TypeMismatchError> {
        let mut ii = msg.iter_init();
        $( let $n = ii.read()?; )*
        Ok(($( $n, )* ))
    }

    fn append(self, msg: &mut Message) {
        let ( $($n,)*) = self;
        let mut ia = IterAppend::new(msg);
        $( ia.append($n); )*
    }
}

impl<$($t: Append),*> AppendAll for ($($t,)*) {
    fn append(self, ia: &mut IterAppend) {
        let ( $($n,)*) = self;
        $( ia.append($n); )*
    }
}

impl<$($t: Arg + for<'z> Get<'z>),*> ReadAll for ($($t,)*) {
    fn read(ii: &mut Iter) -> Result<Self, TypeMismatchError> {
        $( let $n = ii.read()?; )*
        Ok(($( $n, )* ))
    }
}


    }
}

argbuilder_impl!(a A str,);
argbuilder_impl!(a A str, b B str,);
argbuilder_impl!(a A str, b B str, c C str,);
argbuilder_impl!(a A str, b B str, c C str, d D str,);
argbuilder_impl!(a A str, b B str, c C str, d D str, e E str,);
argbuilder_impl!(a A str, b B str, c C str, d D str, e E str, f F str,);
argbuilder_impl!(a A str, b B str, c C str, d D str, e E str, f F str, g G str,);
argbuilder_impl!(a A str, b B str, c C str, d D str, e E str, f F str, g G str, h H str,);
argbuilder_impl!(a A str, b B str, c C str, d D str, e E str, f F str, g G str, h H str, i I str,);
argbuilder_impl!(a A str, b B str, c C str, d D str, e E str, f F str, g G str, h H str, i I str, j J str,);
