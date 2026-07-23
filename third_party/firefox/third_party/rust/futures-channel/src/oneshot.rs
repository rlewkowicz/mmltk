//! A channel for sending a single message between asynchronous tasks.
//!
//! This is a single-producer, single-consumer channel.

use alloc::sync::Arc;
use core::fmt;
use core::pin::Pin;
use core::sync::atomic::AtomicBool;
use core::sync::atomic::Ordering::SeqCst;
use futures_core::future::{FusedFuture, Future};
use futures_core::task::{Context, Poll, Waker};

use crate::lock::Lock;

/// A future for a value that will be provided by another asynchronous task.
///
/// This is created by the [`channel`] function.
#[must_use = "futures do nothing unless you `.await` or poll them"]
pub struct Receiver<T> {
    inner: Arc<Inner<T>>,
}

/// A means of transmitting a single value to another task.
///
/// This is created by the [`channel`] function.
pub struct Sender<T> {
    inner: Arc<Inner<T>>,
}

impl<T> Unpin for Receiver<T> {}
impl<T> Unpin for Sender<T> {}

/// Internal state of the `Receiver`/`Sender` pair above. This is all used as
/// the internal synchronization between the two for send/recv operations.
struct Inner<T> {
    /// Indicates whether this oneshot is complete yet. This is filled in both
    /// by `Sender::drop` and by `Receiver::drop`, and both sides interpret it
    /// appropriately.
    ///
    /// For `Receiver`, if this is `true`, then it's guaranteed that `data` is
    /// unlocked and ready to be inspected.
    ///
    /// For `Sender` if this is `true` then the oneshot has gone away and it
    /// can return ready from `poll_canceled`.
    complete: AtomicBool,

    /// The actual data being transferred as part of this `Receiver`. This is
    /// filled in by `Sender::complete` and read by `Receiver::poll`.
    ///
    /// Note that this is protected by `Lock`, but it is in theory safe to
    /// replace with an `UnsafeCell` as it's actually protected by `complete`
    /// above. I wouldn't recommend doing this, however, unless someone is
    /// supremely confident in the various atomic orderings here and there.
    data: Lock<Option<T>>,

    /// Field to store the task which is blocked in `Receiver::poll`.
    ///
    /// This is filled in when a oneshot is polled but not ready yet. Note that
    /// the `Lock` here, unlike in `data` above, is important to resolve races.
    /// Both the `Receiver` and the `Sender` halves understand that if they
    /// can't acquire the lock then some important interference is happening.
    rx_task: Lock<Option<Waker>>,

    /// Like `rx_task` above, except for the task blocked in
    /// `Sender::poll_canceled`. Additionally, `Lock` cannot be `UnsafeCell`.
    tx_task: Lock<Option<Waker>>,
}

/// Creates a new one-shot channel for sending a single value across asynchronous tasks.
///
/// The channel works for a spsc (single-producer, single-consumer) scheme.
///
/// This function is similar to Rust's channel constructor found in the standard
/// library. Two halves are returned, the first of which is a `Sender` handle,
/// used to signal the end of a computation and provide its value. The second
/// half is a `Receiver` which implements the `Future` trait, resolving to the
/// value that was given to the `Sender` handle.
///
/// Each half can be separately owned and sent across tasks.
///
/// # Examples
///
/// ```
/// use futures::channel::oneshot;
/// use std::{thread, time::Duration};
///
/// let (sender, receiver) = oneshot::channel::<i32>();
///
/// thread::spawn(|| {
///     println!("THREAD: sleeping zzz...");
///     thread::sleep(Duration::from_millis(1000));
///     println!("THREAD: i'm awake! sending.");
///     sender.send(3).unwrap();
/// });
///
/// println!("MAIN: doing some useful stuff");
///
/// futures::executor::block_on(async {
///     println!("MAIN: waiting for msg...");
///     println!("MAIN: got: {:?}", receiver.await)
/// });
/// ```
pub fn channel<T>() -> (Sender<T>, Receiver<T>) {
    let inner = Arc::new(Inner::new());
    let receiver = Receiver { inner: inner.clone() };
    let sender = Sender { inner };
    (sender, receiver)
}

impl<T> Inner<T> {
    fn new() -> Self {
        Self {
            complete: AtomicBool::new(false),
            data: Lock::new(None),
            rx_task: Lock::new(None),
            tx_task: Lock::new(None),
        }
    }

    fn send(&self, t: T) -> Result<(), T> {
        if self.complete.load(SeqCst) {
            return Err(t);
        }

        if let Some(mut slot) = self.data.try_lock() {
            assert!(slot.is_none());
            *slot = Some(t);
            drop(slot);

            if self.complete.load(SeqCst) {
                if let Some(mut slot) = self.data.try_lock() {
                    if let Some(t) = slot.take() {
                        return Err(t);
                    }
                }
            }
            Ok(())
        } else {
            Err(t)
        }
    }

    fn poll_canceled(&self, cx: &mut Context<'_>) -> Poll<()> {
        if self.complete.load(SeqCst) {
            return Poll::Ready(());
        }

        let handle = cx.waker().clone();
        match self.tx_task.try_lock() {
            Some(mut p) => *p = Some(handle),
            None => return Poll::Ready(()),
        }
        if self.complete.load(SeqCst) {
            Poll::Ready(())
        } else {
            Poll::Pending
        }
    }

    fn is_canceled(&self) -> bool {
        self.complete.load(SeqCst)
    }

    fn drop_tx(&self) {
        self.complete.store(true, SeqCst);

        if let Some(mut slot) = self.rx_task.try_lock() {
            if let Some(task) = slot.take() {
                drop(slot);
                task.wake();
            }
        }

        if let Some(mut slot) = self.tx_task.try_lock() {
            drop(slot.take());
        }
    }

    fn close_rx(&self) {
        self.complete.store(true, SeqCst);
        if let Some(mut handle) = self.tx_task.try_lock() {
            if let Some(task) = handle.take() {
                drop(handle);
                task.wake()
            }
        }
    }

    fn try_recv(&self) -> Result<Option<T>, Canceled> {
        if self.complete.load(SeqCst) {
            if let Some(mut slot) = self.data.try_lock() {
                if let Some(data) = slot.take() {
                    return Ok(Some(data));
                }
            }
            Err(Canceled)
        } else {
            Ok(None)
        }
    }

    fn recv(&self, cx: &mut Context<'_>) -> Poll<Result<T, Canceled>> {
        let done = if self.complete.load(SeqCst) {
            true
        } else {
            let task = cx.waker().clone();
            match self.rx_task.try_lock() {
                Some(mut slot) => {
                    *slot = Some(task);
                    false
                }
                None => true,
            }
        };

        if done || self.complete.load(SeqCst) {
            if let Some(mut slot) = self.data.try_lock() {
                if let Some(data) = slot.take() {
                    return Poll::Ready(Ok(data));
                }
            }
            Poll::Ready(Err(Canceled))
        } else {
            Poll::Pending
        }
    }

    fn drop_rx(&self) {
        self.complete.store(true, SeqCst);

        if let Some(mut slot) = self.rx_task.try_lock() {
            let task = slot.take();
            drop(slot);
            drop(task);
        }

        if let Some(mut handle) = self.tx_task.try_lock() {
            if let Some(task) = handle.take() {
                drop(handle);
                task.wake()
            }
        }
    }
}

impl<T> Sender<T> {
    /// Completes this oneshot with a successful result.
    ///
    /// This function will consume `self` and indicate to the other end, the
    /// [`Receiver`], that the value provided is the result of the computation
    /// this represents.
    ///
    /// If the value is successfully enqueued for the remote end to receive,
    /// then `Ok(())` is returned. If the receiving end was dropped before
    /// this function was called, however, then `Err(t)` is returned.
    pub fn send(self, t: T) -> Result<(), T> {
        self.inner.send(t)
    }

    /// Polls this `Sender` half to detect whether its associated
    /// [`Receiver`] has been dropped.
    ///
    /// # Return values
    ///
    /// If `Ready(())` is returned then the associated `Receiver` has been
    /// dropped, which means any work required for sending should be canceled.
    ///
    /// If `Pending` is returned then the associated `Receiver` is still
    /// alive and may be able to receive a message if sent. The current task,
    /// however, is scheduled to receive a notification if the corresponding
    /// `Receiver` goes away.
    pub fn poll_canceled(&mut self, cx: &mut Context<'_>) -> Poll<()> {
        self.inner.poll_canceled(cx)
    }

    /// Creates a future that resolves when this `Sender`'s corresponding
    /// [`Receiver`] half has hung up.
    ///
    /// This is a utility wrapping [`poll_canceled`](Sender::poll_canceled)
    /// to expose a [`Future`].
    pub fn cancellation(&mut self) -> Cancellation<'_, T> {
        Cancellation { inner: self }
    }

    /// Tests to see whether this `Sender`'s corresponding `Receiver`
    /// has been dropped.
    ///
    /// Unlike [`poll_canceled`](Sender::poll_canceled), this function does not
    /// enqueue a task for wakeup upon cancellation, but merely reports the
    /// current state, which may be subject to concurrent modification.
    pub fn is_canceled(&self) -> bool {
        self.inner.is_canceled()
    }

    /// Tests to see whether this `Sender` is connected to the given `Receiver`. That is, whether
    /// they were created by the same call to `channel`.
    pub fn is_connected_to(&self, receiver: &Receiver<T>) -> bool {
        Arc::ptr_eq(&self.inner, &receiver.inner)
    }
}

impl<T> Drop for Sender<T> {
    fn drop(&mut self) {
        self.inner.drop_tx()
    }
}

impl<T> fmt::Debug for Sender<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Sender").field("complete", &self.inner.complete).finish()
    }
}

/// A future that resolves when the receiving end of a channel has hung up.
///
/// This is an `.await`-friendly interface around [`poll_canceled`](Sender::poll_canceled).
#[must_use = "futures do nothing unless you `.await` or poll them"]
#[derive(Debug)]
pub struct Cancellation<'a, T> {
    inner: &'a mut Sender<T>,
}

impl<T> Future for Cancellation<'_, T> {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        self.inner.poll_canceled(cx)
    }
}

/// Error returned from a [`Receiver`] when the corresponding [`Sender`] is
/// dropped.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Canceled;

impl fmt::Display for Canceled {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "oneshot canceled")
    }
}

#[cfg(feature = "std")]
impl std::error::Error for Canceled {}

impl<T> Receiver<T> {
    /// Gracefully close this receiver, preventing any subsequent attempts to
    /// send to it.
    ///
    /// Any `send` operation which happens after this method returns is
    /// guaranteed to fail. After calling this method, you can use
    /// [`Receiver::poll`](core::future::Future::poll) to determine whether a
    /// message had previously been sent.
    pub fn close(&mut self) {
        self.inner.close_rx()
    }

    /// Attempts to receive a message outside of the context of a task.
    ///
    /// Does not schedule a task wakeup or have any other side effects.
    ///
    /// A return value of `None` must be considered immediately stale (out of
    /// date) unless [`close`](Receiver::close) has been called first.
    ///
    /// Returns an error if the sender was dropped.
    pub fn try_recv(&mut self) -> Result<Option<T>, Canceled> {
        self.inner.try_recv()
    }
}

impl<T> Future for Receiver<T> {
    type Output = Result<T, Canceled>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Result<T, Canceled>> {
        self.inner.recv(cx)
    }
}

impl<T> FusedFuture for Receiver<T> {
    fn is_terminated(&self) -> bool {
        if self.inner.complete.load(SeqCst) {
            if let Some(slot) = self.inner.data.try_lock() {
                if slot.is_some() {
                    return false;
                }
            }
            true
        } else {
            false
        }
    }
}

impl<T> Drop for Receiver<T> {
    fn drop(&mut self) {
        self.inner.drop_rx()
    }
}

impl<T> fmt::Debug for Receiver<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Receiver").field("complete", &self.inner.complete).finish()
    }
}
