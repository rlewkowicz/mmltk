//! D-Bus bindings for Rust
//!
//! [D-Bus](http://dbus.freedesktop.org/) is a message bus, and is mainly used in Linux
//! for communication between processes. It is present by default on almost every
//! Linux distribution out there, and runs in two instances - one per session, and one
//! system-wide.
//!
//! In addition to the API documentation, which you're currently reading, you might want to
//! look in the examples directory, which contains many examples and an argument guide.
//! README.md also contain a few quick "getting started" examples.
//!
//! In addition to this crate, there are two companion crates, dbus-codegen for generating Rust
//! code from D-Bus introspection data, and dbus-tokio for integrating D-Bus with [Tokio](http://tokio.rs).
//! However, at the time of this writing, these are far less mature than this crate. 

#![warn(missing_docs)]

extern crate libc;

pub use ffi::DBusBusType as BusType;
pub use connection::DBusNameFlag as NameFlag;
pub use ffi::DBusRequestNameReply as RequestNameReply;
pub use ffi::DBusReleaseNameReply as ReleaseNameReply;
pub use ffi::DBusMessageType as MessageType;

pub use message::{Message, MessageItem, MessageItemArray, FromMessageItem, OwnedFd, ArrayError, ConnPath};
pub use connection::{Connection, ConnectionItems, ConnectionItem, ConnMsgs, MsgHandler, MsgHandlerResult, MsgHandlerType, MessageCallback};
pub use prop::PropHandler;
pub use prop::Props;
pub use watch::{Watch, WatchEvent};
pub use signalargs::SignalArgs;

/// A TypeSig describes the type of a MessageItem.
#[deprecated(note="Use Signature instead")]
pub type TypeSig<'a> = std::borrow::Cow<'a, str>;

use std::ffi::{CString, CStr};
use std::ptr;
use std::os::raw::c_char;

#[allow(missing_docs)]
extern crate libdbus_sys as ffi;
mod message;
mod prop;
mod watch;
mod connection;
mod signalargs;

mod matchrule;
pub use matchrule::MatchRule;

mod strings;
pub use strings::{Signature, Path, Interface, Member, ErrorName, BusName};

pub mod arg;

pub mod stdintf;

pub mod tree;

static INITDBUS: std::sync::Once = std::sync::ONCE_INIT;

fn init_dbus() {
    INITDBUS.call_once(|| {
        if unsafe { ffi::dbus_threads_init_default() } == 0 {
            panic!("Out of memory when trying to initialize D-Bus library!");
        }
    });
}

/// D-Bus Error wrapper.
pub struct Error {
    e: ffi::DBusError,
}

unsafe impl Send for Error {}


unsafe impl Sync for Error {}

fn c_str_to_slice(c: & *const c_char) -> Option<&str> {
    if *c == ptr::null() { None }
    else { std::str::from_utf8( unsafe { CStr::from_ptr(*c).to_bytes() }).ok() }
}

fn to_c_str(n: &str) -> CString { CString::new(n.as_bytes()).unwrap() }

impl Error {

    /// Create a new custom D-Bus Error.
    pub fn new_custom(name: &str, message: &str) -> Error {
        let n = to_c_str(name);
        let m = to_c_str(&message.replace("%","%%"));
        let mut e = Error::empty();

        unsafe { ffi::dbus_set_error(e.get_mut(), n.as_ptr(), m.as_ptr()) };
        e
    }

    fn empty() -> Error {
        init_dbus();
        let mut e = ffi::DBusError {
            name: ptr::null(),
            message: ptr::null(),
            dummy: 0,
            padding1: ptr::null()
        };
        unsafe { ffi::dbus_error_init(&mut e); }
        Error{ e: e }
    }

    /// Error name/type, e g 'org.freedesktop.DBus.Error.Failed'
    pub fn name(&self) -> Option<&str> {
        c_str_to_slice(&self.e.name)
    }

    /// Custom message, e g 'Could not find a matching object path'
    pub fn message(&self) -> Option<&str> {
        c_str_to_slice(&self.e.message)
    }

    fn get_mut(&mut self) -> &mut ffi::DBusError { &mut self.e }
}

impl Drop for Error {
    fn drop(&mut self) {
        unsafe { ffi::dbus_error_free(&mut self.e); }
    }
}

impl std::fmt::Debug for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> Result<(), std::fmt::Error> {
        write!(f, "D-Bus error: {} ({})", self.message().unwrap_or(""),
            self.name().unwrap_or(""))
    }
}

impl std::error::Error for Error {
    fn description(&self) -> &str { "D-Bus error" }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> Result<(),std::fmt::Error> {
        if let Some(x) = self.message() {
             write!(f, "{:?}", x.to_string())
        } else { Ok(()) }
    }
}

impl From<arg::TypeMismatchError> for Error {
    fn from(t: arg::TypeMismatchError) -> Error {
        Error::new_custom("org.freedesktop.DBus.Error.Failed", &format!("{}", t))
    }
}

impl From<tree::MethodErr> for Error {
    fn from(t: tree::MethodErr) -> Error {
        Error::new_custom(t.errorname(), t.description())
    }
}
