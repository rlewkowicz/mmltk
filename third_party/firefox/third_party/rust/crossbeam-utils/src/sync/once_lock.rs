
use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use std::sync::Once;

pub(crate) struct OnceLock<T> {
    once: Once,
    value: UnsafeCell<MaybeUninit<T>>,
}

unsafe impl<T: Sync + Send> Sync for OnceLock<T> {}
unsafe impl<T: Send> Send for OnceLock<T> {}

impl<T> OnceLock<T> {
    /// Creates a new empty cell.
    #[must_use]
    pub(crate) const fn new() -> Self {
        Self {
            once: Once::new(),
            value: UnsafeCell::new(MaybeUninit::uninit()),
        }
    }

    /// Gets the contents of the cell, initializing it with `f` if the cell
    /// was empty.
    ///
    /// Many threads may call `get_or_init` concurrently with different
    /// initializing functions, but it is guaranteed that only one function
    /// will be executed.
    ///
    /// # Panics
    ///
    /// If `f` panics, the panic is propagated to the caller, and the cell
    /// remains uninitialized.
    ///
    /// It is an error to reentrantly initialize the cell from `f`. The
    /// exact outcome is unspecified. Current implementation deadlocks, but
    /// this may be changed to a panic in the future.
    pub(crate) fn get_or_init<F>(&self, f: F) -> &T
    where
        F: FnOnce() -> T,
    {
        if self.once.is_completed() {
            return unsafe { self.get_unchecked() };
        }
        self.initialize(f);

        unsafe { self.get_unchecked() }
    }

    #[cold]
    fn initialize<F>(&self, f: F)
    where
        F: FnOnce() -> T,
    {
        let slot = self.value.get();

        self.once.call_once(|| {
            let value = f();
            unsafe { slot.write(MaybeUninit::new(value)) }
        });
    }

    /// # Safety
    ///
    /// The value must be initialized
    unsafe fn get_unchecked(&self) -> &T {
        debug_assert!(self.once.is_completed());
        &*self.value.get().cast::<T>()
    }
}

impl<T> Drop for OnceLock<T> {
    fn drop(&mut self) {
        if self.once.is_completed() {
            unsafe { (*self.value.get()).assume_init_drop() };
        }
    }
}
