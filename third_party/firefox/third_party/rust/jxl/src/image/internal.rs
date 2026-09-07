// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use std::{
    alloc::{Layout, alloc, alloc_zeroed, dealloc},
    fmt::Debug,
    mem::MaybeUninit,
    ptr::null_mut,
};

use crate::{
    error::{Error, Result},
    util::{CACHE_LINE_BYTE_SIZE, tracing_wrappers::*},
};

use super::Rect;

#[derive(Debug, Clone, Copy)]
pub(super) struct RawImageBuffer {
    buf: *mut MaybeUninit<u8>,
    bytes_per_row: usize,
    num_rows: usize,
    bytes_between_rows: usize,
}

unsafe impl Send for RawImageBuffer {}

unsafe impl Sync for RawImageBuffer {}

impl RawImageBuffer {
    pub(super) fn check_vals(num_rows: usize, bytes_per_row: usize, bytes_between_rows: usize) {
        if num_rows > 0 {
            assert!(bytes_per_row > 0);
            assert!(bytes_between_rows >= bytes_per_row);
            assert!(
                bytes_between_rows
                    .checked_mul(num_rows - 1)
                    .unwrap()
                    .checked_add(bytes_per_row)
                    .unwrap()
                    <= isize::MAX as usize
            );
        }
    }

    /// Checks that self.buf, self.bytes_per_row, self.bytes_between_rows are all multiple of align.
    /// This guarantees that `self.get_row()` and `self.get_row_mut()` return slices aligned to
    /// `align`.
    #[inline(always)]
    pub(super) fn is_aligned(&self, align: usize) -> bool {
        self.bytes_per_row.is_multiple_of(align)
            && self.bytes_between_rows.is_multiple_of(align)
            && (self.buf as usize).is_multiple_of(align)
    }

    /// Creates a new RawImageBuffer from raw pointers.
    /// It is guaranteed that `buf` will never be used to write uninitialized data.
    ///
    /// # Safety
    /// - `buf` must be valid for reads for all bytes in the range
    ///   `buf[i*bytes_between_rows..i*bytes_between_rows+bytes_per_row]` for all values of `i`
    ///   from `0` to `num_rows-1`.
    /// - The bytes in these ranges must not be accessed as long as the returned `Self` is in scope.
    /// - All the bytes in those ranges (and in between) must be part of the same allocated object.
    ///
    /// Note: these safety requirements match those of JxlOutputBuffer::new_from_ptr, except we
    /// only request validity for reads.
    pub(super) unsafe fn new_from_ptr(
        buf: *mut MaybeUninit<u8>,
        num_rows: usize,
        bytes_per_row: usize,
        bytes_between_rows: usize,
    ) -> Self {
        RawImageBuffer::check_vals(num_rows, bytes_per_row, bytes_between_rows);
        Self {
            buf,
            bytes_per_row,
            bytes_between_rows,
            num_rows,
        }
    }

    fn empty() -> Self {
        RawImageBuffer {
            buf: null_mut(),
            bytes_per_row: 0,
            num_rows: 0,
            bytes_between_rows: 0,
        }
    }

    /// Returns the minimum size that the allocation containing the image data must have, or 0 if
    /// this is an empty image.
    pub(super) fn minimum_allocation_size(&self) -> usize {
        if self.num_rows == 0 {
            0
        } else {
            (self.num_rows - 1) * self.bytes_between_rows + self.bytes_per_row
        }
    }

    #[inline(always)]
    pub(super) fn byte_size(&self) -> (usize, usize) {
        (self.bytes_per_row, self.num_rows)
    }

    /// # Safety
    /// - No uninit data must be written to the returned slice.
    /// - The caller must ensure that ownership rules are respected (for example, because they
    ///   have exclusive access to the data).
    #[inline(always)]
    pub(super) unsafe fn row_mut(&mut self, row: usize) -> &mut [MaybeUninit<u8>] {
        unsafe { self.distinct_rows_mut([row])[0] }
    }

    /// Note: this is quadratic in the number of rows.
    /// # Safety
    /// - No uninit data must be written to the returned slice.
    /// - The caller must ensure that ownership rules are respected (for example, because they
    ///   have exclusive access to the data).
    #[inline(always)]
    pub(super) unsafe fn distinct_rows_mut<I: DistinctRowsIndexes>(
        &mut self,
        rows: I,
    ) -> I::Output<'_, MaybeUninit<u8>> {
        unsafe { rows.get_rows_mut(self) }
    }

    /// # Safety
    /// The caller must ensure that ownership and lifetime rules are respected (for example,
    /// because they have shared access to the data).
    #[inline(always)]
    pub(super) unsafe fn row(&self, row: usize) -> &[MaybeUninit<u8>] {
        assert!(row < self.num_rows);
        let start = row * self.bytes_between_rows;
        let start = unsafe { self.buf.add(start) };
        unsafe { std::slice::from_raw_parts(start, self.bytes_per_row) }
    }

    /// Extracts a sub-rectangle from this buffer. Rectangle coordinates are in bytes.
    /// Safety note: the returned RawImageBuffer retains the same kind of access (read or write) as `self`.
    pub(super) fn rect(&self, rect: Rect) -> Self {
        if rect.size.0 == 0 || rect.size.1 == 0 {
            return Self::empty();
        }
        if cfg!(debug_assertions) {
            rect.check_within(self.byte_size());
        }
        assert!(rect.origin.1 < self.num_rows);
        assert!(rect.origin.1.checked_add(rect.size.1).unwrap() <= self.num_rows);
        assert!(rect.origin.0 < self.bytes_per_row);
        assert!(rect.origin.0.checked_add(rect.size.0).unwrap() <= self.bytes_per_row);
        let start_ptr = unsafe {
            self.buf
                .add(rect.origin.1 * self.bytes_between_rows + rect.origin.0)
        };
        unsafe { Self::new_from_ptr(start_ptr, rect.size.1, rect.size.0, self.bytes_between_rows) }
    }

    /// Returns zeroed memory if `uninit` is `false`, otherwise it returns uninitialized
    /// memory. The returned buffer is aligned to CACHE_LINE_BYTE_SIZE bytes.
    /// The returned RawImageBuffer owns the memory it references, which belongs to a single
    /// allocation of size minimum_allocation_size().
    pub(super) fn try_allocate(byte_size: (usize, usize), uninit: bool) -> Result<RawImageBuffer> {
        let (bytes_per_row, num_rows) = byte_size;
        if bytes_per_row == 0 || num_rows == 0 {
            return Ok(RawImageBuffer::empty());
        }
        if bytes_per_row as u64 >= i64::MAX as u64 / 4 || num_rows as u64 >= i64::MAX as u64 / 4 {
            return Err(Error::ImageSizeTooLarge(bytes_per_row, num_rows));
        }
        debug!("trying to allocate image");
        let bytes_between_rows =
            bytes_per_row.div_ceil(CACHE_LINE_BYTE_SIZE) * CACHE_LINE_BYTE_SIZE;
        let allocation_len = (num_rows - 1)
            .checked_mul(bytes_between_rows)
            .unwrap()
            .checked_add(bytes_per_row)
            .unwrap();
        assert_ne!(allocation_len, 0);
        let layout = Layout::from_size_align(allocation_len, CACHE_LINE_BYTE_SIZE).unwrap();
        let memory = unsafe {
            if uninit {
                alloc(layout)
            } else {
                alloc_zeroed(layout)
            }
        };
        if memory.is_null() {
            return Err(Error::ImageOutOfMemory(bytes_per_row, num_rows));
        }
        Ok(unsafe {
            RawImageBuffer::new_from_ptr(
                memory as *mut MaybeUninit<u8>,
                num_rows,
                bytes_per_row,
                bytes_between_rows,
            )
        })
    }

    /// Returns a copy of the current buffer contents in a new buffer that owns the returned data.
    /// The data is allocated with `try_allocate` so that it matches the size of the current image.
    ///
    /// This function is meant to be used when `self` is an owned buffer, and will panic if the
    /// bytes between rows of a newly allocated image with the same size does not match the value
    /// for `self`.
    ///
    /// # Safety
    /// The caller must ensure that the data referenced by self -- *all*
    /// self.minimum_allocation_size() bytes starting from self.buf, not just the accessible bytes
    /// -- can be read.
    pub(super) unsafe fn try_clone(&self) -> Result<Self> {
        let out = RawImageBuffer::try_allocate(self.byte_size(), true)?;
        assert_eq!(self.bytes_per_row, out.bytes_per_row);
        assert_eq!(self.bytes_between_rows, out.bytes_between_rows);
        assert_eq!(self.num_rows, out.num_rows);
        let data_len = self.minimum_allocation_size();
        assert_eq!(
            self.minimum_allocation_size(),
            out.minimum_allocation_size()
        );
        if data_len != 0 {
            unsafe {
                std::ptr::copy_nonoverlapping(self.buf, out.buf, data_len);
            }
        }
        Ok(out)
    }

    /// Deallocates an owning buffer that was allocated by try_allocate.
    ///
    /// # Safety
    /// The data referenced by `self` must have been allocated with Self::try_allocate.
    pub(super) unsafe fn deallocate(&mut self) {
        if !self.buf.is_null() {
            let allocation_len = self.minimum_allocation_size();
            let layout = Layout::from_size_align(allocation_len, CACHE_LINE_BYTE_SIZE).unwrap();
            unsafe {
                dealloc(self.buf as *mut u8, layout);
            }
        }
    }
}

#[allow(private_interfaces)]
pub trait DistinctRowsIndexes {
    type Output<'a, T: 'static>;

    /// # Safety
    /// - No uninit data must be written to the returned slice.
    /// - The caller must ensure that ownership rules are respected (for example, because they
    ///   have exclusive access to the data).
    unsafe fn get_rows_mut<'a>(
        &self,
        image: &'a mut RawImageBuffer,
    ) -> Self::Output<'a, MaybeUninit<u8>>;

    /// # Safety
    /// - The rows are properly aligned
    /// - The rows contain data that is valid for type T (and thus initialized).
    unsafe fn transmute_rows<'a, T: 'static>(
        rows: Self::Output<'a, MaybeUninit<u8>>,
    ) -> Self::Output<'a, T>;
}

#[allow(private_interfaces)]
impl<const S: usize> DistinctRowsIndexes for [usize; S] {
    type Output<'a, T: 'static> = [&'a mut [T]; S];

    #[inline(always)]
    unsafe fn get_rows_mut<'a>(
        &self,
        image: &'a mut RawImageBuffer,
    ) -> Self::Output<'a, MaybeUninit<u8>> {
        for i in 0..S {
            assert!(self[i] < image.num_rows);
            for j in i + 1..S {
                assert_ne!(self[i], self[j]);
            }
        }
        let start = self.map(|row| row * image.bytes_between_rows);
        let start = start.map(|start| {
            unsafe { image.buf.add(start) }
        });
        start.map(|start| {
            unsafe { std::slice::from_raw_parts_mut(start, image.bytes_per_row) }
        })
    }

    #[inline(always)]
    unsafe fn transmute_rows<'a, T: 'static>(
        rows: Self::Output<'a, MaybeUninit<u8>>,
    ) -> Self::Output<'a, T> {
        rows.map(|row| {
            unsafe {
                std::slice::from_raw_parts_mut(
                    row.as_mut_ptr().cast::<T>(),
                    row.len() / std::mem::size_of::<T>(),
                )
            }
        })
    }
}
