/*!
A lazily initialized value for safe sharing between threads.

The principal type in this module is `Lazy`, which makes it easy to construct
values that are shared safely across multiple threads simultaneously.
*/

use core::fmt;

/// A lazily initialized value that implements `Deref` for `T`.
///
/// A `Lazy` takes an initialization function and permits callers from any
/// thread to access the result of that initialization function in a safe
/// manner. In effect, this permits one-time initialization of global resources
/// in a (possibly) multi-threaded program.
///
/// This type and its functionality are available even when neither the `alloc`
/// nor the `std` features are enabled. In exchange, a `Lazy` does **not**
/// guarantee that the given `create` function is called at most once. It
/// might be called multiple times. Moreover, a call to `Lazy::get` (either
/// explicitly or implicitly via `Lazy`'s `Deref` impl) may block until a `T`
/// is available.
///
/// This is very similar to `lazy_static` or `once_cell`, except it doesn't
/// guarantee that the initialization function will be run once and it works
/// in no-alloc no-std environments. With that said, if you need stronger
/// guarantees or a more flexible API, then it is recommended to use either
/// `lazy_static` or `once_cell`.
///
/// # Warning: may use a spin lock
///
/// When this crate is compiled _without_ the `alloc` feature, then this type
/// may used a spin lock internally. This can have subtle effects that may
/// be undesirable. See [Spinlocks Considered Harmful][spinharm] for a more
/// thorough treatment of this topic.
///
/// [spinharm]: https://matklad.github.io/2020/01/02/spinlocks-considered-harmful.html
///
/// # Example
///
/// This type is useful for creating regexes once, and then using them from
/// multiple threads simultaneously without worrying about synchronization.
///
/// ```
/// use regex_automata::{dfa::regex::Regex, util::lazy::Lazy, Match};
///
/// static RE: Lazy<Regex> = Lazy::new(|| Regex::new("foo[0-9]+bar").unwrap());
///
/// let expected = Some(Match::must(0, 3..14));
/// assert_eq!(expected, RE.find(b"zzzfoo12345barzzz"));
/// ```
pub struct Lazy<T, F = fn() -> T>(lazy::Lazy<T, F>);

impl<T, F> Lazy<T, F> {
    /// Create a new `Lazy` value that is initialized via the given function.
    ///
    /// The `T` type is automatically inferred from the return type of the
    /// `create` function given.
    pub const fn new(create: F) -> Lazy<T, F> {
        Lazy(lazy::Lazy::new(create))
    }
}

impl<T, F: Fn() -> T> Lazy<T, F> {
    /// Return a reference to the lazily initialized value.
    ///
    /// This routine may block if another thread is initializing a `T`.
    ///
    /// Note that given a `x` which has type `Lazy`, this must be called via
    /// `Lazy::get(x)` and not `x.get()`. This routine is defined this way
    /// because `Lazy` impls `Deref` with a target of `T`.
    ///
    /// # Panics
    ///
    /// This panics if the `create` function inside this lazy value panics.
    /// If the panic occurred in another thread, then this routine _may_ also
    /// panic (but is not guaranteed to do so).
    pub fn get(this: &Lazy<T, F>) -> &T {
        this.0.get()
    }
}

impl<T, F: Fn() -> T> core::ops::Deref for Lazy<T, F> {
    type Target = T;

    fn deref(&self) -> &T {
        Lazy::get(self)
    }
}

impl<T: fmt::Debug, F: Fn() -> T> fmt::Debug for Lazy<T, F> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        self.0.fmt(f)
    }
}

#[cfg(feature = "alloc")]
mod lazy {
    use core::{
        fmt,
        marker::PhantomData,
        sync::atomic::{AtomicPtr, Ordering},
    };

    use alloc::boxed::Box;

    /// A non-std lazy initialized value.
    ///
    /// This might run the initialization function more than once, but will
    /// never block.
    ///
    /// I wish I could get these semantics into the non-alloc non-std Lazy
    /// type below, but I'm not sure how to do it. If you can do an alloc,
    /// then the implementation becomes very simple if you don't care about
    /// redundant work precisely because a pointer can be atomically swapped.
    ///
    /// Perhaps making this approach work in the non-alloc non-std case
    /// requires asking the caller for a pointer? It would make the API less
    /// convenient I think.
    pub(super) struct Lazy<T, F> {
        data: AtomicPtr<T>,
        create: F,
        owned: PhantomData<Box<T>>,
    }

    unsafe impl<T: Send + Sync, F: Send + Sync> Sync for Lazy<T, F> {}

    impl<T, F> Lazy<T, F> {
        /// Create a new alloc but non-std lazy value that is racily
        /// initialized. That is, the 'create' function may be called more than
        /// once.
        pub(super) const fn new(create: F) -> Lazy<T, F> {
            Lazy {
                data: AtomicPtr::new(core::ptr::null_mut()),
                create,
                owned: PhantomData,
            }
        }
    }

    impl<T, F: Fn() -> T> Lazy<T, F> {
        /// Get the underlying lazy value. If it hasn't been initialized
        /// yet, then always attempt to initialize it (even if some other
        /// thread is initializing it) and atomically attach it to this lazy
        /// value before returning it.
        pub(super) fn get(&self) -> &T {
            if let Some(data) = self.poll() {
                return data;
            }
            let data = (self.create)();
            let mut ptr = Box::into_raw(Box::new(data));
            let result = self.data.compare_exchange(
                core::ptr::null_mut(),
                ptr,
                Ordering::AcqRel,
                Ordering::Acquire,
            );
            if let Err(old) = result {
                drop(unsafe { Box::from_raw(ptr) });
                ptr = old;
            }
            unsafe { &*ptr }
        }

        /// If this lazy value has been initialized successfully, then return
        /// that value. Otherwise return None immediately. This never attempts
        /// to run initialization itself.
        fn poll(&self) -> Option<&T> {
            let ptr = self.data.load(Ordering::Acquire);
            if ptr.is_null() {
                return None;
            }
            Some(unsafe { &*ptr })
        }
    }

    impl<T: fmt::Debug, F: Fn() -> T> fmt::Debug for Lazy<T, F> {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            f.debug_struct("Lazy").field("data", &self.poll()).finish()
        }
    }

    impl<T, F> Drop for Lazy<T, F> {
        fn drop(&mut self) {
            let ptr = *self.data.get_mut();
            if !ptr.is_null() {
                drop(unsafe { Box::from_raw(ptr) });
            }
        }
    }
}

#[cfg(not(feature = "alloc"))]
mod lazy {
    use core::{
        cell::Cell,
        fmt,
        mem::MaybeUninit,
        panic::{RefUnwindSafe, UnwindSafe},
        sync::atomic::{AtomicU8, Ordering},
    };

    /// Our 'Lazy' value can be in one of three states:
    ///
    /// * INIT is where it starts, and also ends up back here if the
    /// 'create' routine panics.
    /// * BUSY is where it sits while initialization is running in exactly
    /// one thread.
    /// * DONE is where it sits after 'create' has completed and 'data' has
    /// been fully initialized.
    const LAZY_STATE_INIT: u8 = 0;
    const LAZY_STATE_BUSY: u8 = 1;
    const LAZY_STATE_DONE: u8 = 2;

    /// A non-alloc non-std lazy initialized value.
    ///
    /// This guarantees initialization only happens once, but uses a spinlock
    /// to block in the case of simultaneous access. Blocking occurs so that
    /// one thread waits while another thread initializes the value.
    ///
    /// I would much rather have the semantics of the 'alloc' Lazy type above.
    /// Namely, that we might run the initialization function more than once,
    /// but we never otherwise block. However, I don't know how to do that in
    /// a non-alloc non-std context.
    pub(super) struct Lazy<T, F> {
        state: AtomicU8,
        create: Cell<Option<F>>,
        data: Cell<MaybeUninit<T>>,
    }

    unsafe impl<T: Send + Sync, F: Send + Sync> Sync for Lazy<T, F> {}
    impl<T: UnwindSafe, F: UnwindSafe + RefUnwindSafe> RefUnwindSafe
        for Lazy<T, F>
    {
    }

    impl<T, F> Lazy<T, F> {
        /// Create a new non-alloc non-std lazy value that is initialized
        /// exactly once on first use using the given function.
        pub(super) const fn new(create: F) -> Lazy<T, F> {
            Lazy {
                state: AtomicU8::new(LAZY_STATE_INIT),
                create: Cell::new(Some(create)),
                data: Cell::new(MaybeUninit::uninit()),
            }
        }
    }

    impl<T, F: FnOnce() -> T> Lazy<T, F> {
        /// Get the underlying lazy value. If it isn't been initialized
        /// yet, then either initialize it or block until some other thread
        /// initializes it. If the 'create' function given to Lazy::new panics
        /// (even in another thread), then this panics too.
        pub(super) fn get(&self) -> &T {
            while self.state.load(Ordering::Acquire) != LAZY_STATE_DONE {
                let result = self.state.compare_exchange(
                    LAZY_STATE_INIT,
                    LAZY_STATE_BUSY,
                    Ordering::AcqRel,
                    Ordering::Acquire,
                );
                if let Ok(_) = result {
                    let create = unsafe {
                        (*self.create.as_ptr()).take().expect(
                            "Lazy's create function panicked, \
                             preventing initialization,
                             poisoning current thread",
                        )
                    };
                    let guard = Guard { state: &self.state };
                    unsafe {
                        (*self.data.as_ptr()).as_mut_ptr().write(create());
                    }
                    core::mem::forget(guard);
                    self.state.store(LAZY_STATE_DONE, Ordering::Release);
                    break;
                }
                core::hint::spin_loop();
            }
            self.poll().unwrap()
        }

        /// If this lazy value has been initialized successfully, then return
        /// that value. Otherwise return None immediately. This never blocks.
        fn poll(&self) -> Option<&T> {
            if self.state.load(Ordering::Acquire) == LAZY_STATE_DONE {
                Some(unsafe { &*(*self.data.as_ptr()).as_ptr() })
            } else {
                None
            }
        }
    }

    impl<T: fmt::Debug, F: FnMut() -> T> fmt::Debug for Lazy<T, F> {
        fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
            f.debug_struct("Lazy")
                .field("state", &self.state.load(Ordering::Acquire))
                .field("create", &"<closure>")
                .field("data", &self.poll())
                .finish()
        }
    }

    impl<T, F> Drop for Lazy<T, F> {
        fn drop(&mut self) {
            if *self.state.get_mut() == LAZY_STATE_DONE {
                unsafe {
                    self.data.get_mut().assume_init_drop();
                }
            }
        }
    }

    /// A guard that will reset a Lazy's state back to INIT when dropped. The
    /// idea here is to 'forget' this guard on success. On failure (when a
    /// panic occurs), the Drop impl runs and causes all in-progress and future
    /// 'get' calls to panic. Without this guard, all in-progress and future
    /// 'get' calls would spin forever. Crashing is much better than getting
    /// stuck in an infinite loop.
    struct Guard<'a> {
        state: &'a AtomicU8,
    }

    impl<'a> Drop for Guard<'a> {
        fn drop(&mut self) {
            self.state.store(LAZY_STATE_INIT, Ordering::Release);
        }
    }
}
