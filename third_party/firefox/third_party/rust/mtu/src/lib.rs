// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

#![cfg_attr(all(coverage_nightly, test), feature(coverage_attribute))]

//! A crate to return the name and maximum transmission unit (MTU) of the local network interface
//! towards a given destination `SocketAddr`, optionally from a given local `SocketAddr`.
//!
//! # Usage
//!
//! This crate exports a single function `interface_and_mtu` that returns the name and
//! [maximum transmission unit (MTU)](https://en.wikipedia.org/wiki/Maximum_transmission_unit)
//! of the outgoing network interface towards a remote destination identified by an `IpAddr`.
//!
//! # Example
//!
//! ```
//! # use std::net::{IpAddr, Ipv4Addr};
//! let destination = IpAddr::V4(Ipv4Addr::LOCALHOST);
//! let (name, mtu): (String, usize) = mtu::interface_and_mtu(destination).unwrap();
//! println!("MTU towards {destination} is {mtu} on {name}");
//! ```
//!
//! # Supported Platforms
//!
//! * Linux
//! * Android
//! * macOS
//! * Windows
//! * FreeBSD
//! * NetBSD
//! * OpenBSD
//! * Solaris
//!
//! # Notes
//!
//! The returned MTU may exceed the maximum IP packet size of 65,535 bytes on some platforms for
//! some remote destinations. (For example, loopback destinations on Windows.)
//!
//! The returned interface name is obtained from the operating system.
//!
//! # Contributing
//!
//! We're happy to receive PRs that improve this crate. Please take a look at our [community
//! guidelines](https://github.com/mozilla/neqo/blob/main/CODE_OF_CONDUCT.md) beforehand.

use std::{
    io::{Error, ErrorKind, Result},
    net::IpAddr,
};

macro_rules! asserted_const_with_type {
    ($name:ident, $t1:ty, $e:expr, $t2:ty) => {
        #[allow(
            clippy::allow_attributes,
            clippy::cast_possible_truncation,
            clippy::cast_possible_wrap,
            reason = "Guarded by the following `const_assert_eq!`."
        )]
        const $name: $t1 = $e as $t1;
        const_assert_eq!($name as $t2, $e);
    };
}

#[cfg(bsd)]
mod bsd;

mod linux;


mod routesocket;

#[cfg(bsd)]
use bsd::interface_and_mtu_impl;
use linux::interface_and_mtu_impl;

/// Prepare a default error.
fn default_err() -> Error {
    Error::new(ErrorKind::NotFound, "Local interface MTU not found")
}

/// Prepare an error for cases that "should never happen".
fn unlikely_err(msg: String) -> Error {
    debug_assert!(false, "{msg}");
    Error::other(msg)
}

/// Align `size` to the next multiple of `align` (which needs to be a power of two).
const fn aligned_by(size: usize, align: usize) -> usize {
    if size == 0 {
        align
    } else {
        1 + ((size - 1) | (align - 1))
    }
}

#[cfg(any(target_os = "tvos", target_os = "visionos", target_os = "redox"))]
pub fn interface_and_mtu_impl(remote: IpAddr) -> Result<(String, usize)> {
    return Err(default_err());
}

/// Return the name and maximum transmission unit (MTU) of the outgoing network interface towards a
/// remote destination identified by an [`IpAddr`],
///
/// The returned MTU may exceed the maximum IP packet size of 65,535 bytes on some platforms for
/// some remote destinations. (For example, loopback destinations on Windows.)
///
/// The returned interface name is obtained from the operating system.
///
/// # Errors
///
/// This function returns an error if the local interface MTU cannot be determined.
pub fn interface_and_mtu(remote: IpAddr) -> Result<(String, usize)> {
    interface_and_mtu_impl(remote)
}
