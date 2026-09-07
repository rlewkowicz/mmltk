//! `futures::task::AtomicWaker` extracted into its own crate.
//!
//! # Features
//!
//! This crate adds a feature, `portable-atomic`, which uses a polyfill
//! from the [`portable-atomic`] crate in order to provide functionality
//! to targets without atomics. See the [`README`] for the [`portable-atomic`]
//! crate for more information on how to use it.
//!
//! [`portable-atomic`]: https://crates.io/crates/portable-atomic
//! [`README`]: https://github.com/taiki-e/portable-atomic/blob/main/README.md#optional-cfg

#![no_std]
#![doc(
    html_favicon_url = "https://raw.githubusercontent.com/smol-rs/smol/master/assets/images/logo_fullsize_transparent.png"
)]
#![doc(
    html_logo_url = "https://raw.githubusercontent.com/smol-rs/smol/master/assets/images/logo_fullsize_transparent.png"
)]

use core::cell::UnsafeCell;
use core::fmt;
use core::sync::atomic::Ordering::{AcqRel, Acquire, Release};
use core::task::Waker;

#[cfg(not(feature = "portable-atomic"))]
use core::sync::atomic::AtomicUsize;
#[cfg(feature = "portable-atomic")]
use portable_atomic::AtomicUsize;

/// A synchronization primitive for task wakeup.
///
/// Sometimes the task interested in a given event will change over time.
/// An `AtomicWaker` can coordinate concurrent notifications with the consumer
/// potentially "updating" the underlying task to wake up. This is useful in
/// scenarios where a computation completes in another thread and wants to
/// notify the consumer, but the consumer is in the process of being migrated to
/// a new logical task.
///
/// Consumers should call `register` before checking the result of a computation
/// and producers should call `wake` after producing the computation (this
/// differs from the usual `thread::park` pattern). It is also permitted for
/// `wake` to be called **before** `register`. This results in a no-op.
///
/// A single `AtomicWaker` may be reused for any number of calls to `register` or
/// `wake`.
///
/// # Memory ordering
///
/// Calling `register` "acquires" all memory "released" by calls to `wake`
/// before the call to `register`.  Later calls to `wake` will wake the
/// registered waker (on contention this wake might be triggered in `register`).
///
/// For concurrent calls to `register` (should be avoided) the ordering is only
/// guaranteed for the winning call.
///
/// # Examples
///
/// Here is a simple example providing a `Flag` that can be signalled manually
/// when it is ready.
///
/// ```
/// use futures::future::Future;
/// use futures::task::{Context, Poll, AtomicWaker};
/// use std::sync::Arc;
/// use std::sync::atomic::AtomicBool;
/// use std::sync::atomic::Ordering::Relaxed;
/// use std::pin::Pin;
///
/// struct Inner {
///     waker: AtomicWaker,
///     set: AtomicBool,
/// }
///
/// #[derive(Clone)]
/// struct Flag(Arc<Inner>);
///
/// impl Flag {
///     pub fn new() -> Self {
///         Flag(Arc::new(Inner {
///             waker: AtomicWaker::new(),
///             set: AtomicBool::new(false),
///         }))
///     }
///
///     pub fn signal(&self) {
///         self.0.set.store(true, Relaxed);
///         self.0.waker.wake();
///     }
/// }
///
/// impl Future for Flag {
///     type Output = ();
///
///     fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
///         // quick check to avoid registration if already done.
///         if self.0.set.load(Relaxed) {
///             return Poll::Ready(());
///         }
///
///         self.0.waker.register(cx.waker());
///
///         // Need to check condition **after** `register` to avoid a race
///         // condition that would result in lost notifications.
///         if self.0.set.load(Relaxed) {
///             Poll::Ready(())
///         } else {
///             Poll::Pending
///         }
///     }
/// }
/// ```
pub struct AtomicWaker {
    state: AtomicUsize,
    waker: UnsafeCell<Option<Waker>>,
}


/// Idle state
const WAITING: usize = 0;

/// A new waker value is being registered with the `AtomicWaker` cell.
const REGISTERING: usize = 0b01;

/// The waker currently registered with the `AtomicWaker` cell is being woken.
const WAKING: usize = 0b10;

impl AtomicWaker {
    /// Create an `AtomicWaker`.
    pub const fn new() -> Self {
        trait AssertSync: Sync {}
        impl AssertSync for Waker {}

        AtomicWaker {
            state: AtomicUsize::new(WAITING),
            waker: UnsafeCell::new(None),
        }
    }

    /// Registers the waker to be notified on calls to `wake`.
    ///
    /// The new task will take place of any previous tasks that were registered
    /// by previous calls to `register`. Any calls to `wake` that happen after
    /// a call to `register` (as defined by the memory ordering rules), will
    /// notify the `register` caller's task and deregister the waker from future
    /// notifications. Because of this, callers should ensure `register` gets
    /// invoked with a new `Waker` **each** time they require a wakeup.
    ///
    /// It is safe to call `register` with multiple other threads concurrently
    /// calling `wake`. This will result in the `register` caller's current
    /// task being notified once.
    ///
    /// This function is safe to call concurrently, but this is generally a bad
    /// idea. Concurrent calls to `register` will attempt to register different
    /// tasks to be notified. One of the callers will win and have its task set,
    /// but there is no guarantee as to which caller will succeed.
    ///
    /// # Examples
    ///
    /// Here is how `register` is used when implementing a flag.
    ///
    /// ```
    /// use futures::future::Future;
    /// use futures::task::{Context, Poll, AtomicWaker};
    /// use std::sync::atomic::AtomicBool;
    /// use std::sync::atomic::Ordering::Relaxed;
    /// use std::pin::Pin;
    ///
    /// struct Flag {
    ///     waker: AtomicWaker,
    ///     set: AtomicBool,
    /// }
    ///
    /// impl Future for Flag {
    ///     type Output = ();
    ///
    ///     fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
    ///         // Register **before** checking `set` to avoid a race condition
    ///         // that would result in lost notifications.
    ///         self.waker.register(cx.waker());
    ///
    ///         if self.set.load(Relaxed) {
    ///             Poll::Ready(())
    ///         } else {
    ///             Poll::Pending
    ///         }
    ///     }
    /// }
    /// ```
    pub fn register(&self, waker: &Waker) {
        match self
            .state
            .compare_exchange(WAITING, REGISTERING, Acquire, Acquire)
            .unwrap_or_else(|x| x)
        {
            WAITING => {
                unsafe {

                    match &*self.waker.get() {
                        Some(old_waker) if old_waker.will_wake(waker) => (),
                        _ => *self.waker.get() = Some(waker.clone()),
                    }

                    let res = self
                        .state
                        .compare_exchange(REGISTERING, WAITING, AcqRel, Acquire);

                    match res {
                        Ok(_) => {
                        }
                        Err(actual) => {
                            debug_assert_eq!(actual, REGISTERING | WAKING);

                            let waker = (*self.waker.get()).take().unwrap();

                            self.state.swap(WAITING, AcqRel);

                            waker.wake();
                        }
                    }
                }
            }
            WAKING => {
                waker.wake_by_ref();
            }
            state => {
                debug_assert!(state == REGISTERING || state == REGISTERING | WAKING);
            }
        }
    }

    /// Calls `wake` on the last `Waker` passed to `register`.
    ///
    /// If `register` has not been called yet, then this does nothing.
    pub fn wake(&self) {
        if let Some(waker) = self.take() {
            waker.wake();
        }
    }

    /// Returns the last `Waker` passed to `register`, so that the user can wake it.
    ///
    ///
    /// Sometimes, just waking the AtomicWaker is not fine grained enough. This allows the user
    /// to take the waker and then wake it separately, rather than performing both steps in one
    /// atomic action.
    ///
    /// If a waker has not been registered, this returns `None`.
    pub fn take(&self) -> Option<Waker> {
        match self.state.fetch_or(WAKING, AcqRel) {
            WAITING => {
                let waker = unsafe { (*self.waker.get()).take() };

                self.state.fetch_and(!WAKING, Release);

                waker
            }
            state => {
                debug_assert!(
                    state == REGISTERING || state == REGISTERING | WAKING || state == WAKING
                );
                None
            }
        }
    }
}

impl Default for AtomicWaker {
    fn default() -> Self {
        AtomicWaker::new()
    }
}

impl fmt::Debug for AtomicWaker {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "AtomicWaker")
    }
}

unsafe impl Send for AtomicWaker {}
unsafe impl Sync for AtomicWaker {}
