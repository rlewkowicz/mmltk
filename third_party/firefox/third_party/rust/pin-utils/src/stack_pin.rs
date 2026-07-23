/// Pins a value on the stack.
///
/// # Example
///
/// ```rust
/// # use pin_utils::pin_mut;
/// # use core::pin::Pin;
/// # struct Foo {}
/// let foo = Foo { /* ... */ };
/// pin_mut!(foo);
/// let _: Pin<&mut Foo> = foo;
/// ```
#[macro_export]
macro_rules! pin_mut {
    ($($x:ident),* $(,)?) => { $(
        let mut $x = $x;
        #[allow(unused_mut)]
        let mut $x = unsafe {
            $crate::core_reexport::pin::Pin::new_unchecked(&mut $x)
        };
    )* }
}
