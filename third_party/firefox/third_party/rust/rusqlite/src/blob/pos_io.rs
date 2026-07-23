use super::Blob;

use std::mem::MaybeUninit;
use std::slice::from_raw_parts_mut;

use crate::ffi;
use crate::{Error, Result};

impl Blob<'_> {
    /// Write `buf` to `self` starting at `write_start`, returning an error if
    /// `write_start + buf.len()` is past the end of the blob.
    ///
    /// If an error is returned, no data is written.
    ///
    /// Note: the blob cannot be resized using this function -- that must be
    /// done using SQL (for example, an `UPDATE` statement).
    ///
    /// Note: This is part of the positional I/O API, and thus takes an absolute
    /// position write to, instead of using the internal position that can be
    /// manipulated by the `std::io` traits.
    ///
    /// Unlike the similarly named [`FileExt::write_at`][fext_write_at] function
    /// (from `std::os::unix`), it's always an error to perform a "short write".
    ///
    /// [fext_write_at]: https://doc.rust-lang.org/std/os/unix/fs/trait.FileExt.html#tymethod.write_at
    #[inline]
    pub fn write_at(&mut self, buf: &[u8], write_start: usize) -> Result<()> {
        let len = self.len();

        if buf.len().saturating_add(write_start) > len {
            return Err(Error::BlobSizeError);
        }
        debug_assert!(i32::try_from(write_start).is_ok() && i32::try_from(buf.len()).is_ok());
        self.conn.decode_result(unsafe {
            ffi::sqlite3_blob_write(
                self.blob,
                buf.as_ptr().cast(),
                buf.len() as i32,
                write_start as i32,
            )
        })
    }

    /// An alias for `write_at` provided for compatibility with the conceptually
    /// equivalent [`std::os::unix::FileExt::write_all_at`][write_all_at]
    /// function from libstd:
    ///
    /// [write_all_at]: https://doc.rust-lang.org/std/os/unix/fs/trait.FileExt.html#method.write_all_at
    #[inline]
    pub fn write_all_at(&mut self, buf: &[u8], write_start: usize) -> Result<()> {
        self.write_at(buf, write_start)
    }

    /// Read as much as possible from `offset` to `offset + buf.len()` out of
    /// `self`, writing into `buf`. On success, returns the number of bytes
    /// written.
    ///
    /// If there's insufficient data in `self`, then the returned value will be
    /// less than `buf.len()`.
    ///
    /// See also [`Blob::raw_read_at`], which can take an uninitialized buffer,
    /// or [`Blob::read_at_exact`] which returns an error if the entire `buf` is
    /// not read.
    ///
    /// Note: This is part of the positional I/O API, and thus takes an absolute
    /// position to read from, instead of using the internal position that can
    /// be manipulated by the `std::io` traits. Consequently, it does not change
    /// that value either.
    #[inline]
    pub fn read_at(&self, buf: &mut [u8], read_start: usize) -> Result<usize> {
        let as_uninit: &mut [MaybeUninit<u8>] =
            unsafe { from_raw_parts_mut(buf.as_mut_ptr().cast(), buf.len()) };
        self.raw_read_at(as_uninit, read_start).map(|s| s.len())
    }

    /// Read as much as possible from `offset` to `offset + buf.len()` out of
    /// `self`, writing into `buf`. On success, returns the portion of `buf`
    /// which was initialized by this call.
    ///
    /// If there's insufficient data in `self`, then the returned value will be
    /// shorter than `buf`.
    ///
    /// See also [`Blob::read_at`], which takes a `&mut [u8]` buffer instead of
    /// a slice of `MaybeUninit<u8>`.
    ///
    /// Note: This is part of the positional I/O API, and thus takes an absolute
    /// position to read from, instead of using the internal position that can
    /// be manipulated by the `std::io` traits. Consequently, it does not change
    /// that value either.
    #[inline]
    pub fn raw_read_at<'a>(
        &self,
        buf: &'a mut [MaybeUninit<u8>],
        read_start: usize,
    ) -> Result<&'a mut [u8]> {
        let len = self.len();

        let read_len = match len.checked_sub(read_start) {
            None | Some(0) => 0,
            Some(v) => v.min(buf.len()),
        };

        if read_len == 0 {
            let empty = unsafe { from_raw_parts_mut(buf.as_mut_ptr().cast::<u8>(), 0) };
            return Ok(empty);
        }

        debug_assert!(i32::try_from(read_start).is_ok());

        debug_assert!(read_start + read_len <= len);

        debug_assert!(buf.len() >= read_len);
        debug_assert!(i32::try_from(buf.len()).is_ok());
        debug_assert!(i32::try_from(read_len).is_ok());

        unsafe {
            self.conn.decode_result(ffi::sqlite3_blob_read(
                self.blob,
                buf.as_mut_ptr().cast(),
                read_len as i32,
                read_start as i32,
            ))?;

            Ok(from_raw_parts_mut(buf.as_mut_ptr().cast::<u8>(), read_len))
        }
    }

    /// Equivalent to [`Blob::read_at`], but returns a `BlobSizeError` if `buf`
    /// is not fully initialized.
    #[inline]
    pub fn read_at_exact(&self, buf: &mut [u8], read_start: usize) -> Result<()> {
        let n = self.read_at(buf, read_start)?;
        if n != buf.len() {
            Err(Error::BlobSizeError)
        } else {
            Ok(())
        }
    }

    /// Equivalent to [`Blob::raw_read_at`], but returns a `BlobSizeError` if
    /// `buf` is not fully initialized.
    #[inline]
    pub fn raw_read_at_exact<'a>(
        &self,
        buf: &'a mut [MaybeUninit<u8>],
        read_start: usize,
    ) -> Result<&'a mut [u8]> {
        let buflen = buf.len();
        let initted = self.raw_read_at(buf, read_start)?;
        if initted.len() != buflen {
            Err(Error::BlobSizeError)
        } else {
            Ok(initted)
        }
    }
}
