// Copyright 2016 Amanieu d'Antras
// Licensed under the Apache License, Version 2.0, <LICENSE-APACHE or
// http://apache.org/licenses/LICENSE-2.0> or the MIT license <LICENSE-MIT or
// http://opensource.org/licenses/MIT>, at your option. This file may not be
// copied, modified, or distributed except according to those terms.

use crate::util::UncheckedOptionExt;
use core::{
    fmt, mem,
    sync::atomic::{fence, AtomicU8, Ordering},
};
use parking_lot_core::{self, SpinWait, DEFAULT_PARK_TOKEN, DEFAULT_UNPARK_TOKEN};

const DONE_BIT: u8 = 1;
const POISON_BIT: u8 = 2;
const LOCKED_BIT: u8 = 4;
const PARKED_BIT: u8 = 8;

/// Current state of a `Once`.
#[derive(Copy, Clone, Eq, PartialEq, Debug)]
pub enum OnceState {
    /// A closure has not been executed yet
    New,

    /// A closure was executed but panicked.
    Poisoned,

    /// A thread is currently executing a closure.
    InProgress,

    /// A closure has completed successfully.
    Done,
}

impl OnceState {
    /// Returns whether the associated `Once` has been poisoned.
    ///
    /// Once an initialization routine for a `Once` has panicked it will forever
    /// indicate to future forced initialization routines that it is poisoned.
    #[inline]
    pub fn poisoned(self) -> bool {
        matches!(self, OnceState::Poisoned)
    }

    /// Returns whether the associated `Once` has successfully executed a
    /// closure.
    #[inline]
    pub fn done(self) -> bool {
        matches!(self, OnceState::Done)
    }
}

/// A synchronization primitive which can be used to run a one-time
/// initialization. Useful for one-time initialization for globals, FFI or
/// related functionality.
///
/// # Differences from the standard library `Once`
///
/// - Only requires 1 byte of space, instead of 1 word.
/// - Not required to be `'static`.
/// - Relaxed memory barriers in the fast path, which can significantly improve
///   performance on some architectures.
/// - Efficient handling of micro-contention using adaptive spinning.
///
/// # Examples
///
/// ```
/// use parking_lot::Once;
///
/// static START: Once = Once::new();
///
/// START.call_once(|| {
///     // run initialization here
/// });
/// ```
pub struct Once(AtomicU8);

impl Once {
    /// Creates a new `Once` value.
    #[inline]
    pub const fn new() -> Once {
        Once(AtomicU8::new(0))
    }

    /// Returns the current state of this `Once`.
    #[inline]
    pub fn state(&self) -> OnceState {
        let state = self.0.load(Ordering::Acquire);
        if state & DONE_BIT != 0 {
            OnceState::Done
        } else if state & LOCKED_BIT != 0 {
            OnceState::InProgress
        } else if state & POISON_BIT != 0 {
            OnceState::Poisoned
        } else {
            OnceState::New
        }
    }

    /// Performs an initialization routine once and only once. The given closure
    /// will be executed if this is the first time `call_once` has been called,
    /// and otherwise the routine will *not* be invoked.
    ///
    /// This method will block the calling thread if another initialization
    /// routine is currently running.
    ///
    /// When this function returns, it is guaranteed that some initialization
    /// has run and completed (it may not be the closure specified). It is also
    /// guaranteed that any memory writes performed by the executed closure can
    /// be reliably observed by other threads at this point (there is a
    /// happens-before relation between the closure and code executing after the
    /// return).
    ///
    /// # Examples
    ///
    /// ```
    /// use parking_lot::Once;
    ///
    /// static mut VAL: usize = 0;
    /// static INIT: Once = Once::new();
    ///
    /// // Accessing a `static mut` is unsafe much of the time, but if we do so
    /// // in a synchronized fashion (e.g. write once or read all) then we're
    /// // good to go!
    /// //
    /// // This function will only call `expensive_computation` once, and will
    /// // otherwise always return the value returned from the first invocation.
    /// fn get_cached_val() -> usize {
    ///     unsafe {
    ///         INIT.call_once(|| {
    ///             VAL = expensive_computation();
    ///         });
    ///         VAL
    ///     }
    /// }
    ///
    /// fn expensive_computation() -> usize {
    ///     // ...
    /// # 2
    /// }
    /// ```
    ///
    /// # Panics
    ///
    /// The closure `f` will only be executed once if this is called
    /// concurrently amongst many threads. If that closure panics, however, then
    /// it will *poison* this `Once` instance, causing all future invocations of
    /// `call_once` to also panic.
    #[inline]
    pub fn call_once<F>(&self, f: F)
    where
        F: FnOnce(),
    {
        if self.0.load(Ordering::Acquire) == DONE_BIT {
            return;
        }

        let mut f = Some(f);
        self.call_once_slow(false, &mut |_| unsafe { f.take().unchecked_unwrap()() });
    }

    /// Performs the same function as `call_once` except ignores poisoning.
    ///
    /// If this `Once` has been poisoned (some initialization panicked) then
    /// this function will continue to attempt to call initialization functions
    /// until one of them doesn't panic.
    ///
    /// The closure `f` is yielded a structure which can be used to query the
    /// state of this `Once` (whether initialization has previously panicked or
    /// not).
    #[inline]
    pub fn call_once_force<F>(&self, f: F)
    where
        F: FnOnce(OnceState),
    {
        if self.0.load(Ordering::Acquire) == DONE_BIT {
            return;
        }

        let mut f = Some(f);
        self.call_once_slow(true, &mut |state| unsafe {
            f.take().unchecked_unwrap()(state)
        });
    }

    #[cold]
    fn call_once_slow(&self, ignore_poison: bool, f: &mut dyn FnMut(OnceState)) {
        let mut spinwait = SpinWait::new();
        let mut state = self.0.load(Ordering::Relaxed);
        loop {
            if state & DONE_BIT != 0 {
                fence(Ordering::Acquire);
                return;
            }

            if state & POISON_BIT != 0 && !ignore_poison {
                fence(Ordering::Acquire);
                panic!("Once instance has previously been poisoned");
            }

            if state & LOCKED_BIT == 0 {
                match self.0.compare_exchange_weak(
                    state,
                    (state | LOCKED_BIT) & !POISON_BIT,
                    Ordering::Acquire,
                    Ordering::Relaxed,
                ) {
                    Ok(_) => break,
                    Err(x) => state = x,
                }
                continue;
            }

            if state & PARKED_BIT == 0 && spinwait.spin() {
                state = self.0.load(Ordering::Relaxed);
                continue;
            }

            if state & PARKED_BIT == 0 {
                if let Err(x) = self.0.compare_exchange_weak(
                    state,
                    state | PARKED_BIT,
                    Ordering::Relaxed,
                    Ordering::Relaxed,
                ) {
                    state = x;
                    continue;
                }
            }

            let addr = self as *const _ as usize;
            let validate = || self.0.load(Ordering::Relaxed) == LOCKED_BIT | PARKED_BIT;
            let before_sleep = || {};
            let timed_out = |_, _| unreachable!();
            unsafe {
                parking_lot_core::park(
                    addr,
                    validate,
                    before_sleep,
                    timed_out,
                    DEFAULT_PARK_TOKEN,
                    None,
                );
            }

            spinwait.reset();
            state = self.0.load(Ordering::Relaxed);
        }

        struct PanicGuard<'a>(&'a Once);
        impl<'a> Drop for PanicGuard<'a> {
            fn drop(&mut self) {
                let once = self.0;
                let state = once.0.swap(POISON_BIT, Ordering::Release);
                if state & PARKED_BIT != 0 {
                    let addr = once as *const _ as usize;
                    unsafe {
                        parking_lot_core::unpark_all(addr, DEFAULT_UNPARK_TOKEN);
                    }
                }
            }
        }

        let guard = PanicGuard(self);
        let once_state = if state & POISON_BIT != 0 {
            OnceState::Poisoned
        } else {
            OnceState::New
        };
        f(once_state);
        mem::forget(guard);

        let state = self.0.swap(DONE_BIT, Ordering::Release);
        if state & PARKED_BIT != 0 {
            let addr = self as *const _ as usize;
            unsafe {
                parking_lot_core::unpark_all(addr, DEFAULT_UNPARK_TOKEN);
            }
        }
    }
}

impl Default for Once {
    #[inline]
    fn default() -> Once {
        Once::new()
    }
}

impl fmt::Debug for Once {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Once")
            .field("state", &self.state())
            .finish()
    }
}
