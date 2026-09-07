use std::os::raw::{c_int, c_ulong};

#[macro_use]
mod platform;

pub use platform::*;

extern "C" {
    #[doc(hidden)]
    pub fn ioctl(fd: c_int, req: c_ulong, ...) -> c_int;
}

#[doc(hidden)]
pub fn check_res(res: c_int) -> std::io::Result<()> {
    if res < 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}
