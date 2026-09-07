//! Implementations that just need to read from a file
use crate::Error;
use core::{
    ffi::c_void,
    mem::MaybeUninit,
    sync::atomic::{AtomicI32, Ordering},
};


#[path = "../util_libc.rs"]
pub(super) mod util_libc;

/// For all platforms, we use `/dev/urandom` rather than `/dev/random`.
/// For more information see the linked man pages in lib.rs.
///   - On Linux, "/dev/urandom is preferred and sufficient in all use cases".
///   - On Redox, only /dev/urandom is provided.
///   - On AIX, /dev/urandom will "provide cryptographically secure output".
///   - On Haiku and QNX Neutrino they are identical.
const FILE_PATH: &[u8] = b"/dev/urandom\0";

const FD_UNINIT: libc::c_int = -1;
const FD_ONGOING_INIT: libc::c_int = -2;

static FD: AtomicI32 = AtomicI32::new(FD_UNINIT);

#[inline]
pub fn fill_inner(dest: &mut [MaybeUninit<u8>]) -> Result<(), Error> {
    let mut fd = FD.load(Ordering::Acquire);
    if fd == FD_UNINIT || fd == FD_ONGOING_INIT {
        fd = open_or_wait()?;
    }
    util_libc::sys_fill_exact(dest, |buf| unsafe {
        libc::read(fd, buf.as_mut_ptr().cast::<c_void>(), buf.len())
    })
}

/// Open a file in read-only mode.
///
/// # Panics
/// If `path` does not contain any zeros.
fn open_readonly(path: &[u8]) -> Result<libc::c_int, Error> {
    assert!(path.contains(&0));
    loop {
        let fd = unsafe {
            libc::open(
                path.as_ptr().cast::<libc::c_char>(),
                libc::O_RDONLY | libc::O_CLOEXEC,
            )
        };
        if fd >= 0 {
            return Ok(fd);
        }
        let err = util_libc::last_os_error();
        if err.raw_os_error() != Some(libc::EINTR) {
            return Err(err);
        }
    }
}

#[cold]
#[inline(never)]
fn open_or_wait() -> Result<libc::c_int, Error> {
    loop {
        match FD.load(Ordering::Acquire) {
            FD_UNINIT => {
                let res = FD.compare_exchange_weak(
                    FD_UNINIT,
                    FD_ONGOING_INIT,
                    Ordering::AcqRel,
                    Ordering::Relaxed,
                );
                if res.is_ok() {
                    break;
                }
            }
            FD_ONGOING_INIT => sync::wait(),
            fd => return Ok(fd),
        }
    }

    let res = open_fd();
    let val = match res {
        Ok(fd) => fd,
        Err(_) => FD_UNINIT,
    };
    FD.store(val, Ordering::Release);

sync::wake();

    res
}

fn open_fd() -> Result<libc::c_int, Error> {
sync::wait_until_rng_ready()?;
    let fd = open_readonly(FILE_PATH)?;
    debug_assert!(fd >= 0);
    Ok(fd)
}


mod sync {
    use super::{open_readonly, util_libc::last_os_error, Error, FD, FD_ONGOING_INIT};

    /// Wait for atomic `FD` to change value from `FD_ONGOING_INIT` to something else.
    ///
    /// Futex syscall with `FUTEX_WAIT` op puts the current thread to sleep
    /// until futex syscall with `FUTEX_WAKE` op gets executed for `FD`.
    ///
    /// For more information read: https://www.man7.org/linux/man-pages/man2/futex.2.html
    pub(super) fn wait() {
        let op = libc::FUTEX_WAIT | libc::FUTEX_PRIVATE_FLAG;
        let timeout_ptr = core::ptr::null::<libc::timespec>();
        let ret = unsafe { libc::syscall(libc::SYS_futex, &FD, op, FD_ONGOING_INIT, timeout_ptr) };
        debug_assert!({
            match ret {
                0 => true,
                -1 => last_os_error().raw_os_error() == Some(libc::EAGAIN),
                _ => false,
            }
        });
    }

    /// Wake up all threads which wait for value of atomic `FD` to change.
    pub(super) fn wake() {
        let op = libc::FUTEX_WAKE | libc::FUTEX_PRIVATE_FLAG;
        let ret = unsafe { libc::syscall(libc::SYS_futex, &FD, op, libc::INT_MAX) };
        debug_assert!(ret >= 0);
    }

    pub(super) fn wait_until_rng_ready() -> Result<(), Error> {
        let fd = open_readonly(b"/dev/random\0")?;
        let mut pfd = libc::pollfd {
            fd,
            events: libc::POLLIN,
            revents: 0,
        };

        let res = loop {
            let res = unsafe { libc::poll(&mut pfd, 1, -1) };
            if res >= 0 {
                debug_assert_eq!(res, 1);
                break Ok(());
            }
            let err = last_os_error();
            match err.raw_os_error() {
                Some(libc::EINTR) => continue,
                _ => break Err(err),
            }
        };
        unsafe { libc::close(fd) };
        res
    }
}
