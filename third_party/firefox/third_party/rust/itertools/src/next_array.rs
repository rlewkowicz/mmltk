use core::mem::{self, MaybeUninit};

/// An array of at most `N` elements.
struct ArrayBuilder<T, const N: usize> {
    /// The (possibly uninitialized) elements of the `ArrayBuilder`.
    ///
    /// # Safety
    ///
    /// The elements of `arr[..len]` are valid `T`s.
    arr: [MaybeUninit<T>; N],

    /// The number of leading elements of `arr` that are valid `T`s, len <= N.
    len: usize,
}

impl<T, const N: usize> ArrayBuilder<T, N> {
    /// Initializes a new, empty `ArrayBuilder`.
    pub fn new() -> Self {
        Self {
            arr: [(); N].map(|_| MaybeUninit::uninit()),
            len: 0,
        }
    }

    /// Pushes `value` onto the end of the array.
    ///
    /// # Panics
    ///
    /// This panics if `self.len >= N`.
    #[inline(always)]
    pub fn push(&mut self, value: T) {
        let place = &mut self.arr[self.len];
        *place = MaybeUninit::new(value);
        self.len += 1;
    }

    /// Consumes the elements in the `ArrayBuilder` and returns them as an array
    /// `[T; N]`.
    ///
    /// If `self.len() < N`, this returns `None`.
    pub fn take(&mut self) -> Option<[T; N]> {
        if self.len == N {
            self.len = 0;

            let arr = mem::replace(&mut self.arr, [(); N].map(|_| MaybeUninit::uninit()));

            Some(arr.map(|v| {
                unsafe { v.assume_init() }
            }))
        } else {
            None
        }
    }
}

impl<T, const N: usize> AsMut<[T]> for ArrayBuilder<T, N> {
    fn as_mut(&mut self) -> &mut [T] {
        let valid = &mut self.arr[..self.len];
        unsafe { slice_assume_init_mut(valid) }
    }
}

impl<T, const N: usize> Drop for ArrayBuilder<T, N> {
    fn drop(&mut self) {
        unsafe { core::ptr::drop_in_place(self.as_mut()) }
    }
}

/// Assuming all the elements are initialized, get a mutable slice to them.
///
/// # Safety
///
/// The caller guarantees that the elements `T` referenced by `slice` are in a
/// valid state.
unsafe fn slice_assume_init_mut<T>(slice: &mut [MaybeUninit<T>]) -> &mut [T] {
    unsafe { &mut *(slice as *mut [MaybeUninit<T>] as *mut [T]) }
}

/// Equivalent to `it.next_array()`.
pub(crate) fn next_array<I, const N: usize>(it: &mut I) -> Option<[I::Item; N]>
where
    I: Iterator,
{
    let mut builder = ArrayBuilder::new();
    for _ in 0..N {
        builder.push(it.next()?);
    }
    builder.take()
}
