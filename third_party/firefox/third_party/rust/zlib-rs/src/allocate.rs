#![allow(unpredictable_function_pointer_comparisons)]

use core::ffi::c_int;
use core::{
    alloc::Layout,
    ffi::{c_uint, c_void},
    marker::PhantomData,
    mem,
    ptr::NonNull,
};

#[allow(non_camel_case_types)]
type size_t = usize;

const ALIGN: u8 = 64;
const _: () = assert!(ALIGN.count_ones() == 1);
const _: () = assert!(ALIGN as usize % mem::size_of::<*mut c_void>() == 0);

/// # Safety
///
/// This function is safe, but must have this type signature to be used elsewhere in the library
unsafe extern "C" fn zalloc_c(opaque: *mut c_void, items: c_uint, size: c_uint) -> *mut c_void {
    let _ = opaque;

    extern "C" {
        fn posix_memalign(memptr: *mut *mut c_void, align: size_t, size: size_t) -> c_int;
    }

    let mut ptr = core::ptr::null_mut();
    let size = items as size_t * size as size_t;
    if size == 0 {
        return ptr;
    }
    match unsafe { posix_memalign(&mut ptr, ALIGN.into(), size) } {
        0 => ptr,
        _ => core::ptr::null_mut(),
    }
}

/// # Safety
///
/// This function is safe, but must have this type signature to be used elsewhere in the library

/// # Safety
///
/// This function is safe, but must have this type signature to be used elsewhere in the library
unsafe extern "C" fn zalloc_c_calloc(
    opaque: *mut c_void,
    items: c_uint,
    size: c_uint,
) -> *mut c_void {
    let _ = opaque;

    extern "C" {
        fn calloc(nitems: size_t, size: size_t) -> *mut c_void;
    }

    if items as size_t * size as size_t == 0 {
        return core::ptr::null_mut();
    }

    unsafe { calloc(items as size_t, size as size_t) }
}

/// # Safety
///
/// The `ptr` must be allocated with the allocator that is used internally by `zcfree`
unsafe extern "C" fn zfree_c(opaque: *mut c_void, ptr: *mut c_void) {
    let _ = opaque;

    extern "C" {
        fn free(p: *mut c_void);
    }

    unsafe { free(ptr) }
}

/// # Safety
///
/// This function is safe to call.
#[cfg(feature = "rust-allocator")]
unsafe extern "C" fn zalloc_rust(_opaque: *mut c_void, count: c_uint, size: c_uint) -> *mut c_void {
    let size = count as usize * size as usize;
    if size == 0 {
        return core::ptr::null_mut();
    }

    let layout = Layout::from_size_align(size, ALIGN.into()).unwrap();

    let ptr = unsafe { std::alloc::alloc(layout) };

    ptr as *mut c_void
}

/// # Safety
///
/// This function is safe to call.
#[cfg(feature = "rust-allocator")]
unsafe extern "C" fn zalloc_rust_calloc(
    _opaque: *mut c_void,
    count: c_uint,
    size: c_uint,
) -> *mut c_void {
    let size = count as usize * size as usize;
    if size == 0 {
        return core::ptr::null_mut();
    }

    let layout = Layout::from_size_align(size, ALIGN.into()).unwrap();

    let ptr = unsafe { std::alloc::alloc_zeroed(layout) };

    ptr as *mut c_void
}

/// # Safety
///
/// - `ptr` must be allocated with the rust `alloc::alloc` allocator
/// - `opaque` is a `&usize` that represents the size of the allocation
#[cfg(feature = "rust-allocator")]
unsafe extern "C" fn zfree_rust(opaque: *mut c_void, ptr: *mut c_void) {
    if ptr.is_null() {
        return;
    }

    debug_assert!(!opaque.is_null());
    if opaque.is_null() {
        return;
    }

    let size = unsafe { *(opaque as *mut usize) };

    if size == 0 {
        return;
    }

    let layout = Layout::from_size_align(size, ALIGN.into());
    let layout = layout.unwrap();

    unsafe { std::alloc::dealloc(ptr.cast(), layout) };
}



#[derive(Clone, Copy)]
#[repr(C)]
pub struct Allocator<'a> {
    pub zalloc: crate::c_api::alloc_func,
    pub zfree: crate::c_api::free_func,
    pub opaque: crate::c_api::voidpf,
    pub _marker: PhantomData<&'a ()>,
}

unsafe impl Sync for Allocator<'static> {}

#[cfg(feature = "rust-allocator")]
pub static RUST: Allocator<'static> = Allocator {
    zalloc: zalloc_rust,
    zfree: zfree_rust,
    opaque: core::ptr::null_mut(),
    _marker: PhantomData,
};

#[cfg(feature = "c-allocator")]
pub static C: Allocator<'static> = Allocator {
    zalloc: zalloc_c,
    zfree: zfree_c,
    opaque: core::ptr::null_mut(),
    _marker: PhantomData,
};


impl Allocator<'_> {
    fn allocate_layout(&self, layout: Layout) -> *mut c_void {
        assert!(layout.align() <= ALIGN.into());

        #[cfg(feature = "rust-allocator")]
        if self.zalloc == RUST.zalloc {
            let ptr = unsafe { (RUST.zalloc)(self.opaque, layout.size() as _, 1) };

            debug_assert_eq!(ptr as usize % layout.align(), 0);

            return ptr;
        }



        let extra_space = core::mem::size_of::<*mut c_void>() + layout.align();

        let ptr = unsafe { (self.zalloc)(self.opaque, (layout.size() + extra_space) as _, 1) };

        if ptr.is_null() {
            return ptr;
        }

        let align_diff = (ptr as usize).next_multiple_of(layout.align()) - (ptr as usize);

        let mut return_ptr = unsafe { ptr.cast::<u8>().add(align_diff) };

        if align_diff < core::mem::size_of::<*mut c_void>() {
            let offset = Ord::max(core::mem::size_of::<*mut c_void>(), layout.align());
            return_ptr = unsafe { return_ptr.add(offset) };
        }

        unsafe {
            let original_ptr = return_ptr.sub(core::mem::size_of::<*mut c_void>());
            core::ptr::write_unaligned(original_ptr.cast::<*mut c_void>(), ptr);
        };

        let ptr = return_ptr.cast::<c_void>();

        debug_assert_eq!(ptr as usize % layout.align(), 0);

        ptr
    }

    fn allocate_layout_zeroed(&self, layout: Layout) -> *mut c_void {
        assert!(layout.align() <= ALIGN.into());

        #[cfg(feature = "rust-allocator")]
        if self.zalloc == RUST.zalloc {
            let ptr = unsafe { zalloc_rust_calloc(self.opaque, layout.size() as _, 1) };

            debug_assert_eq!(ptr as usize % layout.align(), 0);

            return ptr;
        }

        #[cfg(feature = "c-allocator")]
        if self.zalloc == C.zalloc {
            let alloc = Allocator {
                zalloc: zalloc_c_calloc,
                zfree: zfree_c,
                opaque: core::ptr::null_mut(),
                _marker: PhantomData,
            };

            return alloc.allocate_layout(layout);
        }

        let ptr = self.allocate_layout(layout);

        if !ptr.is_null() {
            unsafe { core::ptr::write_bytes(ptr, 0u8, layout.size()) };
        }

        ptr
    }

    pub fn allocate_raw<T>(&self) -> Option<NonNull<T>> {
        NonNull::new(self.allocate_layout(Layout::new::<T>()).cast())
    }

    pub fn allocate_slice_raw<T>(&self, len: usize) -> Option<NonNull<T>> {
        NonNull::new(self.allocate_layout(Layout::array::<T>(len).ok()?).cast())
    }

    pub fn allocate_zeroed_raw<T>(&self) -> Option<NonNull<T>> {
        NonNull::new(self.allocate_layout_zeroed(Layout::new::<T>()).cast())
    }

    pub fn allocate_zeroed_buffer(&self, len: usize) -> Option<NonNull<u8>> {
        let layout = Layout::array::<u8>(len).ok()?;
        NonNull::new(self.allocate_layout_zeroed(layout).cast())
    }

    /// # Panics
    ///
    /// - when `len` is 0
    ///
    /// # Safety
    ///
    /// - `ptr` must be allocated with this allocator
    /// - `len` must be the number of `T`s that are in this allocation
    #[allow(unused)] 
    pub unsafe fn deallocate<T>(&self, ptr: *mut T, len: usize) {
        if !ptr.is_null() {
            #[cfg(feature = "rust-allocator")]
            if self.zfree == RUST.zfree {
                assert_ne!(len, 0, "invalid size for {ptr:?}");
                let mut size = core::mem::size_of::<T>() * len;
                return unsafe { (RUST.zfree)(&mut size as *mut usize as *mut c_void, ptr.cast()) };
            }

            unsafe {
                let original_ptr = (ptr as *mut u8).sub(core::mem::size_of::<*const c_void>());
                let free_ptr = core::ptr::read_unaligned(original_ptr as *mut *mut c_void);

                (self.zfree)(self.opaque, free_ptr)
            }
        }
    }
}
