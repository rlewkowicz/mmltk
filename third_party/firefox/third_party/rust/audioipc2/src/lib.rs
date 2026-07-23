// Copyright © 2017 Mozilla Foundation
// This program is made available under an ISC-style license.  See the
// accompanying file LICENSE for details

#![warn(unused_extern_crates)]
#[macro_use]
extern crate log;

pub mod codec;
#[allow(deprecated)]
pub mod errors;
pub mod messages;
pub mod shm;

pub mod ipccore;
pub mod rpccore;
pub mod sys;

pub use crate::messages::{ClientMessage, ServerMessage};

use std::os::unix::io::IntoRawFd;

use std::io::Result;

pub type PlatformHandleType = libc::c_int;

#[derive(Debug)]
pub struct PlatformHandle(PlatformHandleType);

pub const INVALID_HANDLE_VALUE: PlatformHandleType = -1isize as PlatformHandleType;

fn valid_handle(handle: PlatformHandleType) -> bool {
    handle >= 0
}


impl PlatformHandle {
    pub fn new(raw: PlatformHandleType) -> PlatformHandle {
        assert!(valid_handle(raw));
        PlatformHandle(raw)
    }


pub fn from<T: IntoRawFd>(from: T) -> PlatformHandle {
        PlatformHandle::new(from.into_raw_fd())
    }

    #[allow(clippy::missing_safety_doc)]
    pub unsafe fn into_raw(self) -> PlatformHandleType {
        let handle = self.0;
        std::mem::forget(self);
        handle
    }

pub fn duplicate(h: PlatformHandleType) -> Result<PlatformHandle> {
        unsafe {
            let newfd = libc::dup(h);
            if !valid_handle(newfd) {
                return Err(std::io::Error::last_os_error());
            }
            Ok(PlatformHandle::from(newfd))
        }
    }

}

impl Drop for PlatformHandle {
    fn drop(&mut self) {
        unsafe { close_platform_handle(self.0) }
    }
}

unsafe fn close_platform_handle(handle: PlatformHandleType) {
    libc::close(handle);
}






pub fn server_platform_init() {}
