use std::cell::UnsafeCell;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering::SeqCst;

/// A "lock" around data `D`, which employs a *helping* strategy.
///
/// Used to ensure that concurrent `unpark` invocations lead to (1) `poll` being
/// invoked on only a single thread at a time (2) `poll` being invoked at least
/// once after each `unpark` (unless the future has completed).
pub(crate) struct UnparkMutex<D> {
    status: AtomicUsize,

    inner: UnsafeCell<Option<D>>,
}

unsafe impl<D: Send> Send for UnparkMutex<D> {}
unsafe impl<D: Send> Sync for UnparkMutex<D> {}


const WAITING: usize = 0; 

const POLLING: usize = 1; 

const REPOLL: usize = 2; 

const COMPLETE: usize = 3; 

impl<D> UnparkMutex<D> {
    pub(crate) fn new() -> Self {
        Self { status: AtomicUsize::new(WAITING), inner: UnsafeCell::new(None) }
    }

    /// Attempt to "notify" the mutex that a poll should occur.
    ///
    /// An `Ok` result indicates that the `POLLING` state has been entered, and
    /// the caller can proceed to poll the future. An `Err` result indicates
    /// that polling is not necessary (because the task is finished or the
    /// polling has been delegated).
    pub(crate) fn notify(&self) -> Result<D, ()> {
        let mut status = self.status.load(SeqCst);
        loop {
            match status {
                WAITING => {
                    match self.status.compare_exchange(WAITING, POLLING, SeqCst, SeqCst) {
                        Ok(_) => {
                            let data = unsafe {
                                (*self.inner.get()).take().unwrap()
                            };
                            return Ok(data);
                        }
                        Err(cur) => status = cur,
                    }
                }

                POLLING => match self.status.compare_exchange(POLLING, REPOLL, SeqCst, SeqCst) {
                    Ok(_) => return Err(()),
                    Err(cur) => status = cur,
                },

                _ => return Err(()),
            }
        }
    }

    /// Alert the mutex that polling is about to begin, clearing any accumulated
    /// re-poll requests.
    ///
    /// # Safety
    ///
    /// Callable only from the `POLLING`/`REPOLL` states, i.e. between
    /// successful calls to `notify` and `wait`/`complete`.
    pub(crate) unsafe fn start_poll(&self) {
        self.status.store(POLLING, SeqCst);
    }

    /// Alert the mutex that polling completed with `Pending`.
    ///
    /// # Safety
    ///
    /// Callable only from the `POLLING`/`REPOLL` states, i.e. between
    /// successful calls to `notify` and `wait`/`complete`.
    pub(crate) unsafe fn wait(&self, data: D) -> Result<(), D> {
        *self.inner.get() = Some(data);

        match self.status.compare_exchange(POLLING, WAITING, SeqCst, SeqCst) {
            Ok(_) => Ok(()),

            Err(status) => {
                assert_eq!(status, REPOLL);
                self.status.store(POLLING, SeqCst);
                Err((*self.inner.get()).take().unwrap())
            }
        }
    }

    /// Alert the mutex that the task has completed execution and should not be
    /// notified again.
    ///
    /// # Safety
    ///
    /// Callable only from the `POLLING`/`REPOLL` states, i.e. between
    /// successful calls to `notify` and `wait`/`complete`.
    pub(crate) unsafe fn complete(&self) {
        self.status.store(COMPLETE, SeqCst);
    }
}
