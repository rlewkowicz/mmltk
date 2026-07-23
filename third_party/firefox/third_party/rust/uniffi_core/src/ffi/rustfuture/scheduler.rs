/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use std::mem;

use super::{RustFutureContinuationCallback, RustFuturePoll};

/// Schedules a [crate::RustFuture] by managing the continuation data
///
/// This struct manages the continuation callback and data that comes from the foreign side.  It
/// is responsible for calling the continuation callback when the future is ready to be woken up.
///
/// The basic guarantees are:
///
/// * Each callback will be invoked exactly once, with its associated data.
/// * If `wake()` is called, the callback will be invoked to wake up the future -- either
///   immediately or the next time we get a callback.
/// * If `cancel()` is called, the same will happen and the schedule will stay in the cancelled
///   state, invoking any future callbacks as soon as they're stored.

#[derive(Debug)]
pub(super) enum Scheduler {
    /// No continuations set, neither wake() nor cancel() called.
    Empty,
    /// `wake()` was called when there was no continuation set.  The next time `store` is called,
    /// the continuation should be immediately invoked with `RustFuturePoll::Wake`
    Waked,
    /// The future has been cancelled, any future `store` calls should immediately result in the
    /// continuation being called with `RustFuturePoll::Ready`.
    Cancelled,
    /// Continuation set, the next time `wake()`  is called is called, we should invoke it.
    Set(RustFutureContinuationCallback, u64),
}

impl Scheduler {
    pub(super) fn new() -> Self {
        Self::Empty
    }

    /// Store new continuation data if we are in the `Empty` state.  If we are in the `Waked` or
    /// `Cancelled` state, call the continuation immediately with the data.
    pub(super) fn store(&mut self, callback: RustFutureContinuationCallback, data: u64) {
        match self {
            Self::Empty => *self = Self::Set(callback, data),
            Self::Set(old_callback, old_data) => {
                trace!(
                    "store: observed `Self::Set` state.  Is poll() being called from multiple threads at once?"
                );
                old_callback(*old_data, RustFuturePoll::Ready);
                *self = Self::Set(callback, data);
            }
            Self::Waked => {
                *self = Self::Empty;
                callback(data, RustFuturePoll::Wake);
            }
            Self::Cancelled => {
                callback(data, RustFuturePoll::Ready);
            }
        }
    }

    pub(super) fn wake(&mut self) {
        match self {
            Self::Set(callback, old_data) => {
                let old_data = *old_data;
                let callback = *callback;
                *self = Self::Empty;
                callback(old_data, RustFuturePoll::Wake);
            }
            Self::Empty => *self = Self::Waked,
            _ => (),
        }
    }

    pub(super) fn cancel(&mut self) {
        if let Self::Set(callback, old_data) = mem::replace(self, Self::Cancelled) {
            callback(old_data, RustFuturePoll::Ready);
        }
    }

    pub(super) fn is_cancelled(&self) -> bool {
        matches!(self, Self::Cancelled)
    }
}


unsafe impl Send for Scheduler {}
unsafe impl Sync for Scheduler {}
