use core::any::Any;
use core::borrow::Borrow;

use crate::Error;

/// No runtime values provided.
pub const NO_VALUES: &dyn Values = &();

/// Try to find `key` in `values` and then to convert it to `T`.
pub fn get_value<T: Any>(values: &dyn Values, key: impl AsRef<str>) -> Result<&T, Error> {
    let Some(src) = values.get_value(key.as_ref()) else {
        return Err(Error::ValueMissing);
    };

    if let Some(value) = src.downcast_ref::<T>() {
        return Ok(value);
    } else if let Some(value) = src.downcast_ref::<&T>() {
        return Ok(value);
    }

    #[cfg(feature = "alloc")]
    if let Some(value) = src.downcast_ref::<alloc::boxed::Box<T>>() {
        return Ok(value);
    } else if let Some(value) = src.downcast_ref::<alloc::rc::Rc<T>>() {
        return Ok(value);
    } else if let Some(value) = src.downcast_ref::<alloc::sync::Arc<T>>() {
        return Ok(value);
    }

    Err(Error::ValueType)
}

/// A runtime value store for [`Template::render_with_values()`][crate::Template::render_with_values].
pub trait Values {
    /// Try to find `key` in this store.
    fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any>;
}

crate::impl_for_ref! {
    impl Values for T {
        #[inline]
        fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any> {
            T::get_value(self, key)
        }
    }
}

impl Values for () {
    #[inline]
    fn get_value<'a>(&'a self, _: &str) -> Option<&'a dyn Any> {
        None
    }
}

impl<K, V> Values for (K, V)
where
    K: Borrow<str>,
    V: Value,
{
    #[inline]
    fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any> {
        if self.0.borrow() == key {
            self.1.ref_any()
        } else {
            None
        }
    }
}

impl<T: Values> Values for Option<T> {
    #[inline]
    fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any> {
        self.as_ref()?.get_value(key)
    }
}

impl<K, V, const N: usize> Values for [(K, V); N]
where
    K: Borrow<str>,
    V: Value,
{
    #[inline]
    fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any> {
        find_value_linear(self.iter(), key)
    }
}

impl<K, V> Values for [(K, V)]
where
    K: Borrow<str>,
    V: Value,
{
    fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any> {
        find_value_linear(self.iter(), key)
    }
}

#[cfg(feature = "alloc")]
impl<K, V> Values for alloc::vec::Vec<(K, V)>
where
    K: Borrow<str>,
    V: Value,
{
    #[inline]
    fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any> {
        find_value_linear(self.iter(), key)
    }
}

#[cfg(feature = "alloc")]
impl<K, V> Values for alloc::collections::VecDeque<(K, V)>
where
    K: Borrow<str>,
    V: Value,
{
    #[inline]
    fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any> {
        find_value_linear(self.iter(), key)
    }
}

#[cfg(feature = "alloc")]
impl<K, V> Values for alloc::collections::LinkedList<(K, V)>
where
    K: Borrow<str>,
    V: Value,
{
    #[inline]
    fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any> {
        find_value_linear(self.iter(), key)
    }
}

fn find_value_linear<'a, K, V, I>(it: I, key: &str) -> Option<&'a dyn Any>
where
    K: Borrow<str> + 'a,
    V: Value + 'a,
    I: Iterator<Item = &'a (K, V)>,
{
    for (k, v) in it {
        if k.borrow() == key {
            return v.ref_any();
        }
    }
    None
}

#[cfg(feature = "alloc")]
impl<K, V> Values for alloc::collections::BTreeMap<K, V>
where
    K: Borrow<str> + core::cmp::Ord,
    V: Value,
{
    #[inline]
    fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any> {
        self.get(key)?.ref_any()
    }
}

#[cfg(feature = "std")]
impl<K, V, S> Values for std::collections::HashMap<K, V, S>
where
    K: Borrow<str> + Eq + core::hash::Hash,
    V: Value,
    S: core::hash::BuildHasher,
{
    #[inline]
    fn get_value<'a>(&'a self, key: &str) -> Option<&'a dyn Any> {
        self.get(key)?.ref_any()
    }
}

/// A value in a [`Values`] collection.
///
/// This is <code>[dyn](https://doc.rust-lang.org/stable/std/keyword.dyn.html) [Any]</code>,
/// <code>[Option]&lt;dyn Any&gt;</code>, or a reference to either.
pub trait Value {
    /// Returns a reference to this value unless it is `None`.
    fn ref_any(&self) -> Option<&dyn Any>;
}

crate::impl_for_ref! {
    impl Value for T {
        #[inline]
        fn ref_any(&self) -> Option<&dyn Any> {
            T::ref_any(self)
        }
    }
}

impl Value for dyn Any {
    #[inline]
    fn ref_any(&self) -> Option<&dyn Any> {
        Some(self)
    }
}

impl<T: Value> Value for Option<T> {
    #[inline]
    fn ref_any(&self) -> Option<&dyn Any> {
        T::ref_any(self.as_ref()?)
    }
}
