use std::ffi::OsString;
use std::path::PathBuf;

pub fn is_absolute_path(path: OsString) -> Option<PathBuf> {
    let path = PathBuf::from(path);
    if path.is_absolute() {
        Some(path)
    } else {
        None
    }
}

#[cfg(not(target_os = "redox"))]
extern crate libc;

#[cfg(not(target_os = "redox"))]
mod target_unix_not_redox {

use std::env;
use std::ffi::{CStr, OsString};
use std::mem;
use std::os::unix::ffi::OsStringExt;
use std::path::PathBuf;
use std::ptr;

use super::libc;

pub fn home_dir() -> Option<PathBuf> {
    return env::var_os("HOME")
        .and_then(|h| if h.is_empty() { None } else { Some(h) })
        .or_else(|| unsafe { fallback() })
        .map(PathBuf::from);

#[cfg(target_os = "emscripten")]
unsafe fn fallback() -> Option<OsString> {
        None
    }
#[cfg(not(target_os = "emscripten"))]
unsafe fn fallback() -> Option<OsString> {
        let amt = match libc::sysconf(libc::_SC_GETPW_R_SIZE_MAX) {
            n if n < 0 => 512 as usize,
            n => n as usize,
        };
        let mut buf = Vec::with_capacity(amt);
        let mut passwd: libc::passwd = mem::zeroed();
        let mut result = ptr::null_mut();
        match libc::getpwuid_r(
            libc::getuid(),
            &mut passwd,
            buf.as_mut_ptr(),
            buf.capacity(),
            &mut result,
        ) {
            0 if !result.is_null() => {
                let ptr = passwd.pw_dir as *const _;
                let bytes = CStr::from_ptr(ptr).to_bytes();
                if bytes.is_empty() {
                    None
                } else {
                    Some(OsStringExt::from_vec(bytes.to_vec()))
                }
            }
            _ => None,
        }
    }
}

}

#[cfg(not(target_os = "redox"))]
pub use self::target_unix_not_redox::home_dir;

#[cfg(target_os = "redox")]
extern crate redox_users;

#[cfg(target_os = "redox")]
mod target_redox {

use std::path::PathBuf;

use super::redox_users::{All, AllUsers, Config};

pub fn home_dir() -> Option<PathBuf> {
    let current_uid = redox_users::get_uid().ok()?;
    let users = AllUsers::basic(Config::default()).ok()?;
    let user = users.get_by_id(current_uid)?;

    Some(PathBuf::from(user.home.clone()))
}

}

#[cfg(target_os = "redox")]
pub use self::target_redox::home_dir;

mod xdg_user_dirs;

mod target_unix_not_mac {

use std::collections::HashMap;
use std::env;
use std::path::{Path, PathBuf};

use super::{home_dir, is_absolute_path};
use super::xdg_user_dirs;

fn user_dir_file(home_dir: &Path) -> PathBuf {
    env::var_os("XDG_CONFIG_HOME").and_then(is_absolute_path).unwrap_or_else(|| home_dir.join(".config")).join("user-dirs.dirs")
}

pub fn user_dir(user_dir_name: &str) -> Option<PathBuf> {
    if let Some(home_dir) = home_dir() {
        xdg_user_dirs::single(&home_dir, &user_dir_file(&home_dir), user_dir_name).remove(user_dir_name)
    } else {
        None
    }
}

pub fn user_dirs(home_dir_path: &Path) -> HashMap<String, PathBuf> {
    xdg_user_dirs::all(home_dir_path, &user_dir_file(home_dir_path))
}

}

pub use self::target_unix_not_mac::{user_dir, user_dirs};
