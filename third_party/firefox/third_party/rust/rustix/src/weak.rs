
#![cfg_attr(linux_raw, allow(unsafe_code))]

//! Support for "weak linkage" to symbols on Unix
//!
//! Some I/O operations we do in libstd require newer versions of OSes but we
//! need to maintain binary compatibility with older releases for now. In order
//! to use the new functionality when available we use this module for
//! detection.
//!
//! One option to use here is weak linkage, but that is unfortunately only
//! really workable on Linux. Hence, use dlsym to get the symbol value at
//! runtime. This is also done for compatibility with older versions of glibc,
//! and to avoid creating dependencies on `GLIBC_PRIVATE` symbols. It assumes
//! that we've been dynamically linked to the library the symbol comes from,
//! but that is currently always the case for things like libpthread/libc.
//!
//! A long time ago this used weak linkage for the `__pthread_get_minstack`
//! symbol, but that caused Debian to detect an unnecessarily strict versioned
//! dependency on libc6 (#23628).

#![allow(dead_code, unused_macros)]
#![allow(clippy::doc_markdown)]

use crate::ffi::CStr;
use core::ffi::c_void;
use core::ptr::null_mut;
use core::sync::atomic::{self, AtomicPtr, Ordering};
use core::{marker, mem};

const NULL: *mut c_void = null_mut();
const INVALID: *mut c_void = 1 as *mut c_void;

macro_rules! weak {
    ($vis:vis fn $name:ident($($t:ty),*) -> $ret:ty) => (
        #[allow(non_upper_case_globals)]
        $vis static $name: $crate::weak::Weak<unsafe extern "C" fn($($t),*) -> $ret> =
            $crate::weak::Weak::new(concat!(stringify!($name), '\0'));
    )
}

pub(crate) struct Weak<F> {
    name: &'static str,
    addr: AtomicPtr<c_void>,
    _marker: marker::PhantomData<F>,
}

impl<F> Weak<F> {
    pub(crate) const fn new(name: &'static str) -> Self {
        Self {
            name,
            addr: AtomicPtr::new(INVALID),
            _marker: marker::PhantomData,
        }
    }

    pub(crate) fn get(&self) -> Option<F> {
        assert_eq!(mem::size_of::<F>(), mem::size_of::<usize>());
        unsafe {
            match self.addr.load(Ordering::Relaxed) {
                INVALID => self.initialize(),
                NULL => None,
                addr => {
                    let func = mem::transmute_copy::<*mut c_void, F>(&addr);
                    atomic::fence(Ordering::Acquire);
                    Some(func)
                }
            }
        }
    }

    #[cold]
    unsafe fn initialize(&self) -> Option<F> {
        let val = fetch(self.name);
        self.addr.store(val, Ordering::Release);

        match val {
            NULL => None,
            addr => Some(mem::transmute_copy::<*mut c_void, F>(&addr)),
        }
    }
}

#[cfg(linux_raw)]
mod libc {
    use core::ptr;
    use linux_raw_sys::ctypes::{c_char, c_void};

pub(super) const RTLD_DEFAULT: *mut c_void = ptr::null_mut();

    extern "C" {
        pub(super) fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    }

}

unsafe fn fetch(name: &str) -> *mut c_void {
    let name = match CStr::from_bytes_with_nul(name.as_bytes()) {
        Ok(c_str) => c_str,
        Err(..) => return null_mut(),
    };
    libc::dlsym(libc::RTLD_DEFAULT, name.as_ptr().cast())
}

#[cfg(not(linux_kernel))]
macro_rules! syscall {
    ($vis:vis fn $name:ident($($arg_name:ident: $t:ty),*) via $_sys_name:ident -> $ret:ty) => (
        $vis unsafe fn $name($($arg_name: $t),*) -> $ret {
            weak! { fn $name($($t),*) -> $ret }

            if let Some(fun) = $name.get() {
                fun($($arg_name),*)
            } else {
                libc_errno::set_errno(libc_errno::Errno(libc::ENOSYS));
                -1
            }
        }
    )
}

#[cfg(linux_kernel)]
macro_rules! syscall {
    ($vis:vis fn $name:ident($($arg_name:ident: $t:ty),*) via $sys_name:ident -> $ret:ty) => (
        $vis unsafe fn $name($($arg_name:$t),*) -> $ret {
            use libc::*;

            #[allow(dead_code)]
            trait AsSyscallArg {
                type SyscallArgType;
                fn into_syscall_arg(self) -> Self::SyscallArgType;
            }

            impl<T> AsSyscallArg for *mut T {
                type SyscallArgType = *mut T;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self }
            }
            impl<T> AsSyscallArg for *const T {
                type SyscallArgType = *const T;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self }
            }

            impl AsSyscallArg for $crate::fd::BorrowedFd<'_> {
                type SyscallArgType = ::libc::c_int;
                fn into_syscall_arg(self) -> Self::SyscallArgType {
                    $crate::fd::AsRawFd::as_raw_fd(&self) as _
                }
            }

            impl AsSyscallArg for i8 {
                type SyscallArgType = ::libc::c_int;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self.into() }
            }
            impl AsSyscallArg for u8 {
                type SyscallArgType = ::libc::c_int;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self.into() }
            }
            impl AsSyscallArg for i16 {
                type SyscallArgType = ::libc::c_int;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self.into() }
            }
            impl AsSyscallArg for u16 {
                type SyscallArgType = ::libc::c_int;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self.into() }
            }
            impl AsSyscallArg for i32 {
                type SyscallArgType = ::libc::c_int;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self }
            }
            impl AsSyscallArg for u32 {
                type SyscallArgType = ::libc::c_uint;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self }
            }
            impl AsSyscallArg for usize {
                type SyscallArgType = ::libc::c_ulong;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self as _ }
            }

            #[cfg(target_pointer_width = "64")]
            impl AsSyscallArg for i64 {
                type SyscallArgType = ::libc::c_long;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self }
            }
            #[cfg(target_pointer_width = "64")]
            impl AsSyscallArg for u64 {
                type SyscallArgType = ::libc::c_ulong;
                fn into_syscall_arg(self) -> Self::SyscallArgType { self }
            }


            syscall($sys_name, $($arg_name.into_syscall_arg()),*) as $ret
        }
    )
}

macro_rules! weakcall {
    ($vis:vis fn $name:ident($($arg_name:ident: $t:ty),*) -> $ret:ty) => (
        $vis unsafe fn $name($($arg_name: $t),*) -> $ret {
            weak! { fn $name($($t),*) -> $ret }

            if let Some(fun) = $name.get() {
                fun($($arg_name),*)
            } else {
                libc_errno::set_errno(libc_errno::Errno(libc::ENOSYS));
                -1
            }
        }
    )
}

/// A combination of `weakcall` and `syscall`. Use the libc function if it's
/// available, and fall back to `libc::syscall` otherwise.
macro_rules! weak_or_syscall {
    ($vis:vis fn $name:ident($($arg_name:ident: $t:ty),*) via $sys_name:ident -> $ret:ty) => (
        $vis unsafe fn $name($($arg_name: $t),*) -> $ret {
            weak! { fn $name($($t),*) -> $ret }

            if let Some(fun) = $name.get() {
                fun($($arg_name),*)
            } else {
                syscall! { fn $name($($arg_name: $t),*) via $sys_name -> $ret }
                $name($($arg_name),*)
            }
        }
    )
}
