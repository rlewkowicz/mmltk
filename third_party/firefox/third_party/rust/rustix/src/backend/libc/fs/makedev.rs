#![allow(unused_unsafe)]

use crate::backend::c;
use crate::fs::Dev;

#[cfg(not(any(
    apple,
    solarish,
    target_os = "aix",
    target_os = "android",
    target_os = "emscripten",
)))]
#[inline]
pub(crate) fn makedev(maj: u32, min: u32) -> Dev {
    c::makedev(maj, min)
}

#[cfg(solarish)]
pub(crate) fn makedev(maj: u32, min: u32) -> Dev {
    unsafe { c::makedev(maj, min) }
}



#[cfg(target_os = "emscripten")]
#[inline]
pub(crate) fn makedev(maj: u32, min: u32) -> Dev {
    Dev::from(c::makedev(maj, min))
}

#[cfg(apple)]
#[inline]
pub(crate) fn makedev(maj: u32, min: u32) -> Dev {
    unsafe { c::makedev(maj as i32, min as i32) }
}

#[cfg(target_os = "aix")]
#[inline]
pub(crate) fn makedev(maj: u32, min: u32) -> Dev {
    unsafe { c::makedev(maj, min) }
}

#[cfg(not(any(
    apple,
    freebsdlike,
    target_os = "android",
    target_os = "emscripten",
    target_os = "netbsd",
)))]
#[inline]
pub(crate) fn major(dev: Dev) -> u32 {
    unsafe { c::major(dev) }
}

#[cfg(any(
    apple,
    freebsdlike,
    target_os = "netbsd",
    all(target_os = "android", not(target_pointer_width = "32")),
))]
#[inline]
pub(crate) fn major(dev: Dev) -> u32 {
    (unsafe { c::major(dev) }) as u32
}


#[cfg(target_os = "emscripten")]
#[inline]
pub(crate) fn major(dev: Dev) -> u32 {
    unsafe { c::major(dev as u32) }
}

#[cfg(not(any(
    apple,
    freebsdlike,
    target_os = "android",
    target_os = "emscripten",
    target_os = "netbsd",
)))]
#[inline]
pub(crate) fn minor(dev: Dev) -> u32 {
    unsafe { c::minor(dev) }
}

#[cfg(any(apple, freebsdlike))]
#[inline]
pub(crate) fn minor(dev: Dev) -> u32 {
    (unsafe { c::minor(dev) }) as u32
}


#[cfg(target_os = "emscripten")]
#[inline]
pub(crate) fn minor(dev: Dev) -> u32 {
    unsafe { c::minor(dev as u32) }
}
