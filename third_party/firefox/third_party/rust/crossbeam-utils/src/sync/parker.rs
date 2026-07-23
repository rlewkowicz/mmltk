use crate::primitive::sync::atomic::{AtomicUsize, Ordering::SeqCst};
use crate::primitive::sync::{Arc, Condvar, Mutex};
use std::fmt;
use std::marker::PhantomData;
use std::time::{Duration, Instant};

/// A thread parking primitive.
///
/// Conceptually, each `Parker` has an associated token which is initially not present:
///
/// * The [`park`] method blocks the current thread unless or until the token is available, at
///   which point it automatically consumes the token.
///
/// * The [`park_timeout`] and [`park_deadline`] methods work the same as [`park`], but block for
///   a specified maximum time.
///
/// * The [`unpark`] method atomically makes the token available if it wasn't already. Because the
///   token is initially absent, [`unpark`] followed by [`park`] will result in the second call
///   returning immediately.
///
/// In other words, each `Parker` acts a bit like a spinlock that can be locked and unlocked using
/// [`park`] and [`unpark`].
///
/// # Examples
///
/// ```
/// use std::thread;
/// use std::time::Duration;
/// use crossbeam_utils::sync::Parker;
///
/// let p = Parker::new();
/// let u = p.unparker().clone();
///
/// // Make the token available.
/// u.unpark();
/// // Wakes up immediately and consumes the token.
/// p.park();
///
/// thread::spawn(move || {
///     thread::sleep(Duration::from_millis(500));
///     u.unpark();
/// });
///
/// // Wakes up when `u.unpark()` provides the token.
/// p.park();
/// # std::thread::sleep(std::time::Duration::from_millis(500)); // wait for background threads closed: https://github.com/rust-lang/miri/issues/1371
/// ```
///
/// [`park`]: Parker::park
/// [`park_timeout`]: Parker::park_timeout
/// [`park_deadline`]: Parker::park_deadline
/// [`unpark`]: Unparker::unpark
pub struct Parker {
    unparker: Unparker,
    _marker: PhantomData<*const ()>,
}

unsafe impl Send for Parker {}

impl Default for Parker {
    fn default() -> Self {
        Self {
            unparker: Unparker {
                inner: Arc::new(Inner {
                    state: AtomicUsize::new(EMPTY),
                    lock: Mutex::new(()),
                    cvar: Condvar::new(),
                }),
            },
            _marker: PhantomData,
        }
    }
}

impl Parker {
    /// Creates a new `Parker`.
    ///
    /// # Examples
    ///
    /// ```
    /// use crossbeam_utils::sync::Parker;
    ///
    /// let p = Parker::new();
    /// ```
    ///
    pub fn new() -> Parker {
        Self::default()
    }

    /// Blocks the current thread until the token is made available.
    ///
    /// # Examples
    ///
    /// ```
    /// use crossbeam_utils::sync::Parker;
    ///
    /// let p = Parker::new();
    /// let u = p.unparker().clone();
    ///
    /// // Make the token available.
    /// u.unpark();
    ///
    /// // Wakes up immediately and consumes the token.
    /// p.park();
    /// ```
    pub fn park(&self) {
        self.unparker.inner.park(None);
    }

    /// Blocks the current thread until the token is made available, but only for a limited time.
    ///
    /// # Examples
    ///
    /// ```
    /// use std::time::Duration;
    /// use crossbeam_utils::sync::Parker;
    ///
    /// let p = Parker::new();
    ///
    /// // Waits for the token to become available, but will not wait longer than 500 ms.
    /// p.park_timeout(Duration::from_millis(500));
    /// ```
    pub fn park_timeout(&self, timeout: Duration) {
        match Instant::now().checked_add(timeout) {
            Some(deadline) => self.park_deadline(deadline),
            None => self.park(),
        }
    }

    /// Blocks the current thread until the token is made available, or until a certain deadline.
    ///
    /// # Examples
    ///
    /// ```
    /// use std::time::{Duration, Instant};
    /// use crossbeam_utils::sync::Parker;
    ///
    /// let p = Parker::new();
    /// let deadline = Instant::now() + Duration::from_millis(500);
    ///
    /// // Waits for the token to become available, but will not wait longer than 500 ms.
    /// p.park_deadline(deadline);
    /// ```
    pub fn park_deadline(&self, deadline: Instant) {
        self.unparker.inner.park(Some(deadline))
    }

    /// Returns a reference to an associated [`Unparker`].
    ///
    /// The returned [`Unparker`] doesn't have to be used by reference - it can also be cloned.
    ///
    /// # Examples
    ///
    /// ```
    /// use crossbeam_utils::sync::Parker;
    ///
    /// let p = Parker::new();
    /// let u = p.unparker().clone();
    ///
    /// // Make the token available.
    /// u.unpark();
    /// // Wakes up immediately and consumes the token.
    /// p.park();
    /// ```
    ///
    /// [`park`]: Parker::park
    /// [`park_timeout`]: Parker::park_timeout
    pub fn unparker(&self) -> &Unparker {
        &self.unparker
    }

    /// Converts a `Parker` into a raw pointer.
    ///
    /// # Examples
    ///
    /// ```
    /// use crossbeam_utils::sync::Parker;
    ///
    /// let p = Parker::new();
    /// let raw = Parker::into_raw(p);
    /// # let _ = unsafe { Parker::from_raw(raw) };
    /// ```
    pub fn into_raw(this: Parker) -> *const () {
        Unparker::into_raw(this.unparker)
    }

    /// Converts a raw pointer into a `Parker`.
    ///
    /// # Safety
    ///
    /// This method is safe to use only with pointers returned by [`Parker::into_raw`].
    ///
    /// # Examples
    ///
    /// ```
    /// use crossbeam_utils::sync::Parker;
    ///
    /// let p = Parker::new();
    /// let raw = Parker::into_raw(p);
    /// let p = unsafe { Parker::from_raw(raw) };
    /// ```
    pub unsafe fn from_raw(ptr: *const ()) -> Parker {
        Parker {
            unparker: Unparker::from_raw(ptr),
            _marker: PhantomData,
        }
    }
}

impl fmt::Debug for Parker {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.pad("Parker { .. }")
    }
}

/// Unparks a thread parked by the associated [`Parker`].
pub struct Unparker {
    inner: Arc<Inner>,
}

unsafe impl Send for Unparker {}
unsafe impl Sync for Unparker {}

impl Unparker {
    /// Atomically makes the token available if it is not already.
    ///
    /// This method will wake up the thread blocked on [`park`] or [`park_timeout`], if there is
    /// any.
    ///
    /// # Examples
    ///
    /// ```
    /// use std::thread;
    /// use std::time::Duration;
    /// use crossbeam_utils::sync::Parker;
    ///
    /// let p = Parker::new();
    /// let u = p.unparker().clone();
    ///
    /// thread::spawn(move || {
    ///     thread::sleep(Duration::from_millis(500));
    ///     u.unpark();
    /// });
    ///
    /// // Wakes up when `u.unpark()` provides the token.
    /// p.park();
    /// # std::thread::sleep(std::time::Duration::from_millis(500)); // wait for background threads closed: https://github.com/rust-lang/miri/issues/1371
    /// ```
    ///
    /// [`park`]: Parker::park
    /// [`park_timeout`]: Parker::park_timeout
    pub fn unpark(&self) {
        self.inner.unpark()
    }

    /// Converts an `Unparker` into a raw pointer.
    ///
    /// # Examples
    ///
    /// ```
    /// use crossbeam_utils::sync::{Parker, Unparker};
    ///
    /// let p = Parker::new();
    /// let u = p.unparker().clone();
    /// let raw = Unparker::into_raw(u);
    /// # let _ = unsafe { Unparker::from_raw(raw) };
    /// ```
    pub fn into_raw(this: Unparker) -> *const () {
        Arc::into_raw(this.inner).cast::<()>()
    }

    /// Converts a raw pointer into an `Unparker`.
    ///
    /// # Safety
    ///
    /// This method is safe to use only with pointers returned by [`Unparker::into_raw`].
    ///
    /// # Examples
    ///
    /// ```
    /// use crossbeam_utils::sync::{Parker, Unparker};
    ///
    /// let p = Parker::new();
    /// let u = p.unparker().clone();
    ///
    /// let raw = Unparker::into_raw(u);
    /// let u = unsafe { Unparker::from_raw(raw) };
    /// ```
    pub unsafe fn from_raw(ptr: *const ()) -> Unparker {
        Unparker {
            inner: Arc::from_raw(ptr.cast::<Inner>()),
        }
    }
}

impl fmt::Debug for Unparker {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.pad("Unparker { .. }")
    }
}

impl Clone for Unparker {
    fn clone(&self) -> Unparker {
        Unparker {
            inner: self.inner.clone(),
        }
    }
}

const EMPTY: usize = 0;
const PARKED: usize = 1;
const NOTIFIED: usize = 2;

struct Inner {
    state: AtomicUsize,
    lock: Mutex<()>,
    cvar: Condvar,
}

impl Inner {
    fn park(&self, deadline: Option<Instant>) {
        if self
            .state
            .compare_exchange(NOTIFIED, EMPTY, SeqCst, SeqCst)
            .is_ok()
        {
            return;
        }

        if let Some(deadline) = deadline {
            if deadline <= Instant::now() {
                return;
            }
        }

        let mut m = self.lock.lock().unwrap();

        match self.state.compare_exchange(EMPTY, PARKED, SeqCst, SeqCst) {
            Ok(_) => {}
            Err(NOTIFIED) => {
                let old = self.state.swap(EMPTY, SeqCst);
                assert_eq!(old, NOTIFIED, "park state changed unexpectedly");
                return;
            }
            Err(n) => panic!("inconsistent park_timeout state: {}", n),
        }

        loop {
            m = match deadline {
                None => self.cvar.wait(m).unwrap(),
                Some(deadline) => {
                    let now = Instant::now();
                    if now < deadline {
                        self.cvar.wait_timeout(m, deadline - now).unwrap().0
                    } else {
                        match self.state.swap(EMPTY, SeqCst) {
                            NOTIFIED | PARKED => return,
                            n => panic!("inconsistent park_timeout state: {}", n),
                        };
                    }
                }
            };

            if self
                .state
                .compare_exchange(NOTIFIED, EMPTY, SeqCst, SeqCst)
                .is_ok()
            {
                return;
            }

        }
    }

    pub(crate) fn unpark(&self) {
        match self.state.swap(NOTIFIED, SeqCst) {
            EMPTY => return,    
            NOTIFIED => return, 
            PARKED => {}        
            _ => panic!("inconsistent state in unpark"),
        }

        drop(self.lock.lock().unwrap());
        self.cvar.notify_one();
    }
}
