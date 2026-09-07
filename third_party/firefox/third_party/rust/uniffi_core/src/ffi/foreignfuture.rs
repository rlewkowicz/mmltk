/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! This module defines a Rust Future that wraps an async foreign function call.
//!
//! The general idea is to create a oneshot channel, hand the sender to the foreign side, and
//! await the receiver side on the Rust side.
//!
//! The foreign side should:
//!   * Input a [ForeignFutureCallback] and a `u64` handle in their scaffolding function.
//!     This is the sender, converted to a raw pointer, and an extern "C" function that sends the result.
//!   * Call the [ForeignFutureCallback] when the async function completes with:
//!     * The `u64` handle initially passed in
//!     * The `ForeignFutureResult` struct for the call
//!   * Optionally, set the `ForeignFutureDroppedCallback` value if you want to be notified when the
//!     Future is dropped in Rust.  For languages that support it, this can be hooked up to
//!     cancelling the async task for the method.

use crate::{oneshot, LiftReturn, RustCallStatus};

/// Callback that's passed to a foreign async functions.
///
/// See `LiftReturn` trait for how this is implemented.
pub type ForeignFutureCallback<FfiType> =
    extern "C" fn(oneshot_handle: u64, ForeignFutureResult<FfiType>);

/// C struct that represents the result of a foreign future
#[repr(C)]
pub struct ForeignFutureResult<T> {
    return_value: T,
    call_status: RustCallStatus,
}

/// C callback function that's called when the Rust side of a foreign future is dropped
pub type ForeignFutureDroppedCallback = extern "C" fn(data: u64);

/// C struct that represents a foreign future dropped callback.
#[repr(C)]
pub struct ForeignFutureDroppedCallbackStruct {
    pub callback_data: u64,
    pub callback: ForeignFutureDroppedCallback,
}

impl Default for ForeignFutureDroppedCallbackStruct {
    /// The default value implements a no-op callback.
    /// This will be used for languages that don't set their own callbacks.
    fn default() -> Self {
        extern "C" fn callback(_data: u64) {}
        Self {
            callback_data: 0,
            callback,
        }
    }
}

impl Drop for ForeignFutureDroppedCallbackStruct {
    fn drop(&mut self) {
        (self.callback)(self.callback_data)
    }
}

unsafe impl Send for ForeignFutureDroppedCallbackStruct {}

pub async fn foreign_async_call<F, T, UT>(call_scaffolding_function: F) -> T
where
    F: FnOnce(ForeignFutureCallback<T::ReturnType>, u64, &mut ForeignFutureDroppedCallbackStruct),
    T: LiftReturn<UT>,
{
    let (sender, receiver) = oneshot::channel::<ForeignFutureResult<T::ReturnType>>();
    let complete_callback = foreign_future_complete::<T, UT>;
    let complete_callback_data = sender.into_raw() as u64;
    let mut foreign_future_dropped_callback = ForeignFutureDroppedCallbackStruct::default();
    call_scaffolding_function(
        complete_callback,
        complete_callback_data,
        &mut foreign_future_dropped_callback,
    );
    let result = receiver.await;
    T::lift_foreign_return(result.return_value, result.call_status)
}

pub extern "C" fn foreign_future_complete<T: LiftReturn<UT>, UT>(
    oneshot_handle: u64,
    result: ForeignFutureResult<T::ReturnType>,
) {
    let channel = unsafe { oneshot::Sender::from_raw(oneshot_handle as *mut ()) };
    channel.send(result);
}
