extern crate libc;

use std::marker::PhantomData;
use std::ops::Deref;
use std::slice;
use libc::c_void;

struct MallocPtr(*mut c_void);

impl Drop for MallocPtr {
    fn drop(&mut self) {
        unsafe {
            libc::free(self.0);
        }
    }
}

/// A type that represents a `malloc`'d chunk of memory.
pub struct MallocBuffer<T> {
    ptr: MallocPtr,
    len: usize,
    items: PhantomData<[T]>,
}

impl<T: Copy> MallocBuffer<T> {
    /// Constructs a new `MallocBuffer` for a `malloc`'d buffer
    /// with the given length at the given pointer.
    /// Returns `None` if the given pointer is null and the length is not 0.
    ///
    /// When this `MallocBuffer` drops, the buffer will be `free`'d.
    ///
    /// Unsafe because there must be `len` contiguous, valid instances of `T`
    /// at `ptr`.
    pub unsafe fn new(ptr: *mut T, len: usize) -> Option<MallocBuffer<T>> {
        if len > 0 && ptr.is_null() {
            None
        } else {
            Some(MallocBuffer {
                ptr: MallocPtr(ptr as *mut c_void),
                len: len,
                items: PhantomData,
            })
        }
    }
}

impl<T> Deref for MallocBuffer<T> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        let ptr = if self.len == 0 && self.ptr.0.is_null() {
            0x1 as *const T
        } else {
            self.ptr.0 as *const T
        };
        unsafe {
            slice::from_raw_parts(ptr, self.len)
        }
    }
}
