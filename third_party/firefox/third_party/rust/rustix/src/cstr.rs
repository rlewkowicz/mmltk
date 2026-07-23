/// A macro for [`CStr`] literals.
///
/// This can make passing string literals to rustix APIs more efficient, since
/// most underlying system calls with string arguments expect NUL-terminated
/// strings, and passing strings to rustix as `CStr`s means that rustix doesn't
/// need to copy them into a separate buffer to NUL-terminate them.
///
/// In Rust ≥ 1.77, users can use [C-string literals] instead of this macro.
///
/// [`CStr`]: crate::ffi::CStr
/// [C-string literals]: https://blog.rust-lang.org/2024/03/21/Rust-1.77.0.html#c-string-literals
///
/// # Examples
///
/// ```
/// # #[cfg(feature = "fs")]
/// # fn main() -> rustix::io::Result<()> {
/// use rustix::cstr;
/// use rustix::fs::{statat, AtFlags, CWD};
///
/// let metadata = statat(CWD, cstr!("Cargo.toml"), AtFlags::empty())?;
/// # Ok(())
/// # }
/// # #[cfg(not(feature = "fs"))]
/// # fn main() {}
/// ```
#[allow(unused_macros)]
#[macro_export]
macro_rules! cstr {
    ($str:literal) => {{
        ::core::assert!(
            !::core::iter::Iterator::any(&mut ::core::primitive::str::bytes($str), |b| b == b'\0'),
            "cstr argument contains embedded NUL bytes",
        );

        #[allow(unsafe_code, unused_unsafe)]
        {
            unsafe {
                $crate::ffi::CStr::from_bytes_with_nul_unchecked(
                    ::core::concat!($str, "\0").as_bytes(),
                )
            }
        }
    }};
}
