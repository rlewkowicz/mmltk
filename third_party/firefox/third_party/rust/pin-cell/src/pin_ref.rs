use core::cell::Ref;
use core::fmt;
use core::ops::Deref;
use core::pin::Pin;

#[derive(Debug)]
/// A wrapper type for a immutably borrowed value from a `PinCell<T>`.
pub struct PinRef<'a, T: ?Sized> {
    pub(crate) inner: Pin<Ref<'a, T>>,
}

impl<'a, T: ?Sized> Deref for PinRef<'a, T> {
    type Target = T;

    fn deref(&self) -> &T {
        &*self.inner
    }
}

impl<'a, T: ?Sized> PinRef<'a, T> {
    /// Get a pinned reference to the value inside this wrapper.
    pub fn as_ref<'b>(orig: &'b PinRef<'a, T>) -> Pin<&'b T> {
        orig.inner.as_ref()
    }
}


impl<'a, T: fmt::Display + ?Sized> fmt::Display for PinRef<'a, T> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        <T as fmt::Display>::fmt(&**self, f)
    }
}

