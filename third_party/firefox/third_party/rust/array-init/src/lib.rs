#![no_std]

//! The `array-init` crate allows you to initialize arrays
//! with an initializer closure that will be called
//! once for each element until the array is filled.
//!
//! This way you do not need to default-fill an array
//! before running initializers. Rust currently only
//! lets you either specify all initializers at once,
//! individually (`[a(), b(), c(), ...]`), or specify
//! one initializer for a `Copy` type (`[a(); N]`),
//! which will be called once with the result copied over.
//!
//! Care is taken not to leak memory shall the initialization
//! fail.
//!
//! # Examples:
//! ```rust
//! # #![allow(unused)]
//! # extern crate array_init;
//! #
//! // Initialize an array of length 50 containing
//! // successive squares
//!
//! let arr: [u32; 50] = array_init::array_init(|i: usize| (i * i) as u32);
//!
//! // Initialize an array from an iterator
//! // producing an array of [1,2,3,4] repeated
//!
//! let four = [1,2,3,4];
//! let mut iter = four.iter().copied().cycle();
//! let arr: [u32; 50] = array_init::from_iter(iter).unwrap();
//!
//! // Closures can also mutate state. We guarantee that they will be called
//! // in order from lower to higher indices.
//!
//! let mut last = 1u64;
//! let mut secondlast = 0;
//! let fibonacci: [u64; 50] = array_init::array_init(|_| {
//!     let this = last + secondlast;
//!     secondlast = last;
//!     last = this;
//!     this
//! });
//! ```

use ::core::{
    mem::{self, MaybeUninit},
    ptr, slice,
};

#[inline]
/// Initialize an array given an initializer expression.
///
/// The initializer is given the index of the element. It is allowed
/// to mutate external state; we will always initialize the elements in order.
///
/// # Examples
///
/// ```rust
/// # #![allow(unused)]
/// # extern crate array_init;
/// #
/// // Initialize an array of length 50 containing
/// // successive squares
/// let arr: [usize; 50] = array_init::array_init(|i| i * i);
///
/// assert!(arr.iter().enumerate().all(|(i, &x)| x == i * i));
/// ```
pub fn array_init<F, T, const N: usize>(mut initializer: F) -> [T; N]
where
    F: FnMut(usize) -> T,
{
    enum Unreachable {}

    try_array_init(
        move |i| -> Result<T, Unreachable> { Ok(initializer(i)) },
    )
    .unwrap_or_else(
        |unreachable| match unreachable {  },
    )
}

#[inline]
/// Initialize an array given an iterator
///
/// We will iterate until the array is full or the iterator is exhausted. Returns
/// `None` if the iterator is exhausted before we can fill the array.
///
///   - Once the array is full, extra elements from the iterator (if any)
///     won't be consumed.
///
/// # Examples
///
/// ```rust
/// # #![allow(unused)]
/// # extern crate array_init;
/// #
/// // Initialize an array from an iterator
/// // producing an array of [1,2,3,4] repeated
///
/// let four = [1,2,3,4];
/// let mut iter = four.iter().copied().cycle();
/// let arr: [u32; 10] = array_init::from_iter(iter).unwrap();
/// assert_eq!(arr, [1, 2, 3, 4, 1, 2, 3, 4, 1, 2]);
/// ```
pub fn from_iter<Iterable, T, const N: usize>(iterable: Iterable) -> Option<[T; N]>
where
    Iterable: IntoIterator<Item = T>,
{
    try_array_init_impl::<_, _, T, N, 1>({
        let mut iterator = iterable.into_iter();
        move |_| iterator.next().ok_or(())
    })
    .ok()
}

#[inline]
/// Initialize an array in reverse given an iterator
///
/// We will iterate until the array is full or the iterator is exhausted. Returns
/// `None` if the iterator is exhausted before we can fill the array.
///
///   - Once the array is full, extra elements from the iterator (if any)
///     won't be consumed.
///
/// # Examples
///
/// ```rust
/// # #![allow(unused)]
/// # extern crate array_init;
/// #
/// // Initialize an array from an iterator
/// // producing an array of [4,3,2,1] repeated, finishing with 1.
///
/// let four = [1,2,3,4];
/// let mut iter = four.iter().copied().cycle();
/// let arr: [u32; 10] = array_init::from_iter_reversed(iter).unwrap();
/// assert_eq!(arr, [2, 1, 4, 3, 2, 1, 4, 3, 2, 1]);
/// ```
pub fn from_iter_reversed<Iterable, T, const N: usize>(iterable: Iterable) -> Option<[T; N]>
where
    Iterable: IntoIterator<Item = T>,
{
    try_array_init_impl::<_, _, T, N, -1>({
        let mut iterator = iterable.into_iter();
        move |_| iterator.next().ok_or(())
    })
    .ok()
}

#[inline]
/// Initialize an array given an initializer expression that may fail.
///
/// The initializer is given the index (between 0 and `N - 1` included) of the element, and returns a `Result<T, Err>,`. It is allowed
/// to mutate external state; we will always initialize from lower to higher indices.
///
/// # Examples
///
/// ```rust
/// # #![allow(unused)]
/// # extern crate array_init;
/// #
/// #[derive(PartialEq,Eq,Debug)]
/// struct DivideByZero;
///
/// fn inv(i : usize) -> Result<f64,DivideByZero> {
///     if i == 0 {
///         Err(DivideByZero)
///     } else {
///         Ok(1./(i as f64))
///     }
/// }
///
/// // If the initializer does not fail, we get an initialized array
/// let arr: [f64; 3] = array_init::try_array_init(|i| inv(3-i)).unwrap();
/// assert_eq!(arr,[1./3., 1./2., 1./1.]);
///
/// // The initializer fails
/// let res : Result<[f64;4], DivideByZero> = array_init::try_array_init(|i| inv(3-i));
/// assert_eq!(res,Err(DivideByZero));
/// ```
pub fn try_array_init<Err, F, T, const N: usize>(initializer: F) -> Result<[T; N], Err>
where
    F: FnMut(usize) -> Result<T, Err>,
{
    try_array_init_impl::<Err, F, T, N, 1>(initializer)
}

#[inline]
/// Initialize an array given a source array and a mapping expression. The size of the source array
/// is the same as the size of the returned array.
///
/// The mapper is given an element from the source array and maps it to an element in the
/// destination.
///
/// # Examples
///
/// ```rust
/// # #![allow(unused)]
/// # extern crate array_init;
/// #
/// // Initialize an array of length 50 containing successive squares
/// let arr: [usize; 50] = array_init::array_init(|i| i * i);
///
/// // Map each usize element to a u64 element.
/// let u64_arr: [u64; 50] = array_init::map_array_init(&arr, |element| *element as u64);
///
/// assert!(u64_arr.iter().enumerate().all(|(i, &x)| x == (i * i) as u64));
/// ```
pub fn map_array_init<M, T, U, const N: usize>(source: &[U; N], mut mapper: M) -> [T; N]
where
    M: FnMut(&U) -> T,
{
    array_init(|index| unsafe { mapper(source.get_unchecked(index)) })
}

#[inline]
fn try_array_init_impl<Err, F, T, const N: usize, const D: i8>(
    mut initializer: F,
) -> Result<[T; N], Err>
where
    F: FnMut(usize) -> Result<T, Err>,
{
    if !mem::needs_drop::<T>() {
        let mut array: MaybeUninit<[T; N]> = MaybeUninit::uninit();
        let mut ptr_i = array.as_mut_ptr() as *mut T;

        unsafe {
            if D < 0 {
                ptr_i = ptr_i.add(N);
            }
            for i in 0..N {
                let value_i = initializer(i)?;
                if D < 0 {
                    ptr_i = ptr_i.sub(1);
                }
                ptr_i.write(value_i);
                if D > 0 {
                    ptr_i = ptr_i.add(1);
                }
            }
            Ok(array.assume_init())
        }
    } else {

        /// # Safety
        ///
        ///   - `base_ptr[.. initialized_count]` must be a slice of init elements...
        ///
        ///   - ... that must be sound to `ptr::drop_in_place` if/when
        ///     `UnsafeDropSliceGuard` is dropped: "symbolic ownership"
        struct UnsafeDropSliceGuard<Item> {
            base_ptr: *mut Item,
            initialized_count: usize,
        }

        impl<Item> Drop for UnsafeDropSliceGuard<Item> {
            fn drop(self: &'_ mut Self) {
                unsafe {
                    ptr::drop_in_place(slice::from_raw_parts_mut(
                        self.base_ptr,
                        self.initialized_count,
                    ));
                }
            }
        }

        unsafe {
            let mut array: MaybeUninit<[T; N]> = MaybeUninit::uninit();
            let mut ptr_i = array.as_mut_ptr() as *mut T;
            if D < 0 {
                ptr_i = ptr_i.add(N);
            }
            let mut panic_guard = UnsafeDropSliceGuard {
                base_ptr: ptr_i,
                initialized_count: 0,
            };

            for i in 0..N {
                panic_guard.initialized_count = i;
                let value_i = initializer(i)?;
                if D < 0 {
                    ptr_i = ptr_i.sub(1);
                    panic_guard.base_ptr = ptr_i;
                }
                ptr_i.write(value_i);
                if D > 0 {
                    ptr_i = ptr_i.add(1);
                }
            }
            mem::forget(panic_guard);

            Ok(array.assume_init())
        }
    }
}
