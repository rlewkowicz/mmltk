use ffi;
use libc;
use super::Connection;

use std::mem;
use std::sync::{Mutex, RwLock};
use std::os::unix::io::{RawFd, AsRawFd};
use std::os::raw::{c_void, c_uint};

/// A file descriptor to watch for incoming events (for async I/O).
///
/// # Example
/// ```
/// extern crate libc;
/// extern crate dbus;
/// fn main() {
///     use dbus::{Connection, BusType, WatchEvent};
///     let c = Connection::get_private(BusType::Session).unwrap();
///
///     // Get a list of fds to poll for
///     let mut fds: Vec<_> = c.watch_fds().iter().map(|w| w.to_pollfd()).collect();
///
///     // Poll them with a 1 s timeout
///     let r = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::c_ulong, 1000) };
///     assert!(r >= 0);
///
///     // And handle incoming events
///     for pfd in fds.iter().filter(|pfd| pfd.revents != 0) {
///         for item in c.watch_handle(pfd.fd, WatchEvent::from_revents(pfd.revents)) {
///             // Handle item
///             println!("Received ConnectionItem: {:?}", item);
///         }
///     }
/// }
/// ```

#[repr(C)]
#[derive(Debug, PartialEq, Copy, Clone)]
/// The enum is here for backwards compatibility mostly.
///
/// It should really be bitflags instead.
pub enum WatchEvent {
    /// The fd is readable
    Readable = ffi::DBUS_WATCH_READABLE as isize,
    /// The fd is writable
    Writable = ffi::DBUS_WATCH_WRITABLE as isize,
    /// An error occured on the fd
    Error = ffi::DBUS_WATCH_ERROR as isize,
    /// The fd received a hangup.
    Hangup = ffi::DBUS_WATCH_HANGUP as isize,
}

impl WatchEvent {
    /// After running poll, this transforms the revents into a parameter you can send into `Connection::watch_handle`
    pub fn from_revents(revents: libc::c_short) -> c_uint {
        0 +
        if (revents & libc::POLLIN) != 0 { WatchEvent::Readable as c_uint } else { 0 } +
        if (revents & libc::POLLOUT) != 0 { WatchEvent::Writable as c_uint } else { 0 } +
        if (revents & libc::POLLERR) != 0 { WatchEvent::Error as c_uint } else { 0 } +
        if (revents & libc::POLLHUP) != 0 { WatchEvent::Hangup as c_uint } else { 0 }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
/// A file descriptor, and an indication whether it should be read from, written to, or both.
pub struct Watch {
    fd: RawFd,
    read: bool,
    write: bool,
}

impl Watch {
    /// Get the RawFd this Watch is for
    pub fn fd(&self) -> RawFd { self.fd }
    /// Add POLLIN to events to listen for
    pub fn readable(&self) -> bool { self.read }
    /// Add POLLOUT to events to listen for
    pub fn writable(&self) -> bool { self.write }
    /// Returns the current watch as a libc::pollfd, to use with libc::poll
    pub fn to_pollfd(&self) -> libc::pollfd {
        libc::pollfd { fd: self.fd, revents: 0, events: libc::POLLERR + libc::POLLHUP + 
            if self.readable() { libc::POLLIN } else { 0 } +
            if self.writable() { libc::POLLOUT } else { 0 },
        }
    }
}

impl AsRawFd for Watch {
    fn as_raw_fd(&self) -> RawFd { self.fd }
}

/// Note - internal struct, not to be used outside API. Moving it outside its box will break things.
pub struct WatchList {
    watches: RwLock<Vec<*mut ffi::DBusWatch>>,
    enabled_fds: Mutex<Vec<Watch>>,
    on_update: Mutex<Box<Fn(Watch) + Send>>,
}

impl WatchList {
    pub fn new(c: &Connection, on_update: Box<Fn(Watch) + Send>) -> Box<WatchList> {
        let w = Box::new(WatchList { on_update: Mutex::new(on_update), watches: RwLock::new(vec!()), enabled_fds: Mutex::new(vec!()) });
        if unsafe { ffi::dbus_connection_set_watch_functions(super::connection::conn_handle(c),
            Some(add_watch_cb), Some(remove_watch_cb), Some(toggled_watch_cb), &*w as *const _ as *mut _, None) } == 0 {
            panic!("dbus_connection_set_watch_functions failed");
        }
        w
    }

    pub fn set_on_update(&self, on_update: Box<Fn(Watch) + Send>) { *self.on_update.lock().unwrap() = on_update; }

    pub fn watch_handle(&self, fd: RawFd, flags: c_uint) {
        for &q in self.watches.read().unwrap().iter() {
            let w = self.get_watch(q);
            if w.fd != fd { continue };
            if unsafe { ffi::dbus_watch_handle(q, flags) } == 0 {
                panic!("dbus_watch_handle failed");
            }
            self.update(q);
        };
    }

    pub fn get_enabled_fds(&self) -> Vec<Watch> {
        self.enabled_fds.lock().unwrap().clone()
    }

    fn get_watch(&self, watch: *mut ffi::DBusWatch) -> Watch {
        let mut w = Watch { fd: unsafe { ffi::dbus_watch_get_unix_fd(watch) }, read: false, write: false};
        let enabled = self.watches.read().unwrap().contains(&watch) && unsafe { ffi::dbus_watch_get_enabled(watch) != 0 };
        let flags = unsafe { ffi::dbus_watch_get_flags(watch) };
        if enabled {
            w.read = (flags & WatchEvent::Readable as c_uint) != 0;
            w.write = (flags & WatchEvent::Writable as c_uint) != 0;
        }
        w
    }

    fn update(&self, watch: *mut ffi::DBusWatch) {
        let mut w = self.get_watch(watch);

        for &q in self.watches.read().unwrap().iter() {
            if q == watch { continue };
            let ww = self.get_watch(q);
            if ww.fd != w.fd { continue };
            w.read |= ww.read;
            w.write |= ww.write;
        }

        {
            let mut fdarr = self.enabled_fds.lock().unwrap();

            if w.write || w.read {
                if fdarr.contains(&w) { return; } 
            }
            else if !fdarr.iter().any(|q| w.fd == q.fd) { return; } 

            fdarr.retain(|f| f.fd != w.fd);
            if w.write || w.read { fdarr.push(w) };
        }
        let func = self.on_update.lock().unwrap();
        (*func)(w);
    }
}

extern "C" fn add_watch_cb(watch: *mut ffi::DBusWatch, data: *mut c_void) -> u32 {
    let wlist: &WatchList = unsafe { mem::transmute(data) };
    wlist.watches.write().unwrap().push(watch);
    wlist.update(watch);
    1
}

extern "C" fn remove_watch_cb(watch: *mut ffi::DBusWatch, data: *mut c_void) {
    let wlist: &WatchList = unsafe { mem::transmute(data) };
    wlist.watches.write().unwrap().retain(|w| *w != watch);
    wlist.update(watch);
}

extern "C" fn toggled_watch_cb(watch: *mut ffi::DBusWatch, data: *mut c_void) {
    let wlist: &WatchList = unsafe { mem::transmute(data) };
    wlist.update(watch);
}
