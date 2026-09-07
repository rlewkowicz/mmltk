use zlib_rs::c_api::*;

use crate::gz::GzMode::GZ_READ;
use crate::{
    deflate, deflateEnd, deflateInit2_, deflateReset, inflate, inflateEnd, inflateInit2_,
    inflateReset, prefix, z_off64_t, z_off_t, zlibVersion,
};
use core::cmp::Ordering;
use core::ffi::{c_char, c_int, c_uint, c_void, CStr};
use core::ptr;
use libc::size_t; 
use libc::{O_APPEND, O_CREAT, O_EXCL, O_RDONLY, O_TRUNC, O_WRONLY, SEEK_CUR, SEEK_END, SEEK_SET};
use zlib_rs::deflate::Strategy;
use zlib_rs::MAX_WBITS;

/// In the zlib C API, this structure exposes just enough of the internal state
/// of an open [`gzFile`] to support the `gzgetc` C macro. Since Rust code won't be
/// using that C macro, we define [`gzFile_s`] as an empty structure.
#[allow(non_camel_case_types)]
pub enum gzFile_s {}

/// File handle for an open gzip file.
#[allow(non_camel_case_types)]
pub type gzFile = *mut gzFile_s;

#[repr(C)]
struct GzState {
    have: c_uint,       
    next: *const Bytef, 
    pos: i64,           


    mode: GzMode,
    fd: c_int, 
    source: Source,
    want: usize,     
    input: *mut u8,  
    in_size: usize,  
    output: *mut u8, 
    out_size: usize, 
    direct: bool,    

    how: How,
    start: i64,
    eof: bool,  
    past: bool, 

    level: i8,
    strategy: Strategy,
    reset: bool, 

    skip: i64,  
    seek: bool, 

    err: c_int,         
    msg: *const c_char, 

    stream: z_stream,
}

impl GzState {
    fn configure(&mut self, mode: &[u8]) -> Result<(bool, bool), ()> {
        let mut exclusive = false;
        let mut cloexec = false;

        for &ch in mode {
            if ch.is_ascii_digit() {
                self.level = (ch - b'0') as i8;
            } else {
                match ch {
                    b'r' => self.mode = GzMode::GZ_READ,
                    b'w' => self.mode = GzMode::GZ_WRITE,
                    b'a' => self.mode = GzMode::GZ_APPEND,
                    b'+' => {
                        return Err(());
                    }
                    b'b' => {} 
                    b'e' => cloexec = true,
                    b'x' => exclusive = true,
                    b'f' => self.strategy = Strategy::Filtered,
                    b'h' => self.strategy = Strategy::HuffmanOnly,
                    b'R' => self.strategy = Strategy::Rle,
                    b'F' => self.strategy = Strategy::Fixed,
                    b'T' => self.direct = true,
                    _ => {} 
                }
            }
        }

        Ok((exclusive, cloexec))
    }

    fn in_capacity(&self) -> usize {
        match self.mode {
            GzMode::GZ_WRITE => self.want * 2,
            _ => self.want,
        }
    }

    fn out_capacity(&self) -> usize {
        match self.mode {
            GzMode::GZ_READ => self.want * 2,
            _ => self.want,
        }
    }

    /// Compute the number of bytes of input buffered in `self`.
    ///
    /// # Safety
    ///
    /// Either
    /// - `state.input` is null.
    /// - `state.stream.next_in .. state.stream.next_in + state.stream.avail_in`
    ///   is contained in `state.input .. state.input + state.in_size`.
    ///
    /// It is almost always the case that one of those two conditions is true
    /// inside this module. The notable exception is in a specific block within
    /// `gz_write`, where we temporarily set `state.next_in` to point to a
    /// caller-supplied buffer to do a zero-copy optimization when compressing
    /// large inputs.
    unsafe fn input_len(&self) -> usize {
        if self.input.is_null() {
            return 0;
        }

        let end = unsafe { self.stream.next_in.add(self.stream.avail_in as usize) };

        (unsafe { end.offset_from(self.input) }) as _
    }
}

#[allow(non_camel_case_types)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum GzMode {
    GZ_NONE = 0,
    GZ_READ = 7247,
    GZ_WRITE = 31153,
    GZ_APPEND = 1,
}

#[derive(Debug, PartialEq, Eq)]
enum How {
    Look = 0, 
    Copy = 1, 
    Gzip = 2, 
}

const GZBUFSIZE: usize = 128 * 1024;

#[cfg(feature = "rust-allocator")]
use zlib_rs::allocate::RUST as ALLOCATOR;

#[cfg(not(feature = "rust-allocator"))]
#[cfg(feature = "c-allocator")]
use zlib_rs::allocate::C as ALLOCATOR;

#[cfg(not(feature = "rust-allocator"))]
#[cfg(not(feature = "c-allocator"))]
compile_error!("Either rust-allocator or c-allocator feature is required");

enum Source {
    Path(*const c_char),
    Fd(c_int),
}

/// Open a gzip file for reading or writing.
///
/// # Returns
///
/// * If successful, an opaque handle that the caller can later free with [`gzfree`]
/// * On error, a null pointer
///
/// # Safety
///
/// The caller must ensure that `path` and `mode` point to valid C strings. If the
/// return value is non-NULL, caller must delete it using only [`gzclose`].
///
/// [`gzfree`]: crate::z_stream
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzopen64))]
pub unsafe extern "C" fn gzopen64(path: *const c_char, mode: *const c_char) -> gzFile {
    if path.is_null() {
        return ptr::null_mut();
    }
    let source = Source::Path(path);
    unsafe { gzopen_help(source, mode) }
}

/// Open a gzip file for reading or writing.
///
/// # Returns
///
/// * If successful, an opaque handle that the caller can later free with [`gzfree`]
/// * On error, a null pointer
///
/// # Safety
///
/// The caller must ensure that `path` and `mode` point to valid C strings. If the
/// return value is non-NULL, caller must delete it using only [`gzclose`].
///
/// [`gzfree`]: crate::z_stream
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzopen))]
pub unsafe extern "C" fn gzopen(path: *const c_char, mode: *const c_char) -> gzFile {
    if path.is_null() {
        return ptr::null_mut();
    }
    let source = Source::Path(path);
    unsafe { gzopen_help(source, mode) }
}

/// Given an open file descriptor, prepare to read or write a gzip file.
/// NOTE: This is similar to [`gzopen`], but for cases where the caller already
/// has the file open.
///
/// # Returns
///
/// * If successful, an opaque handle that the caller can later free with [`gzfree`]
/// * On error, a null pointer
///
/// # Safety
///
/// The caller must ensure that `mode` points to a valid C string. If the
/// return value is non-NULL, caller must delete it using only [`gzclose`].
///
/// [`gzfree`]: crate::z_stream
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzdopen))]
pub unsafe extern "C" fn gzdopen(fd: c_int, mode: *const c_char) -> gzFile {
    unsafe { gzopen_help(Source::Fd(fd), mode) }
}

/// Internal implementation shared by gzopen and gzdopen.
///
/// # Safety
///
/// The caller must ensure that mode points to a valid C string.
unsafe fn gzopen_help(source: Source, mode: *const c_char) -> gzFile {
    if mode.is_null() {
        return ptr::null_mut();
    }

    let Some(state) = ALLOCATOR.allocate_zeroed_raw::<GzState>() else {
        return ptr::null_mut();
    };
    let state = unsafe { state.cast::<GzState>().as_mut() };
    state.in_size = 0;
    state.out_size = 0;
    state.want = GZBUFSIZE;
    state.msg = ptr::null();

    state.mode = GzMode::GZ_NONE;
    state.level = crate::Z_DEFAULT_COMPRESSION as i8;
    state.strategy = Strategy::Default;
    state.direct = false;

    state.stream = z_stream::default();
    state.stream.zalloc = Some(ALLOCATOR.zalloc);
    state.stream.zfree = Some(ALLOCATOR.zfree);
    state.stream.opaque = ALLOCATOR.opaque;

    let mode = unsafe { CStr::from_ptr(mode) };
    let Ok((exclusive, cloexec)) = state.configure(mode.to_bytes()) else {
        unsafe { free_state(state) };
        return ptr::null_mut();
    };

    if state.mode == GzMode::GZ_NONE {
        unsafe { free_state(state) };
        return ptr::null_mut();
    }

    if state.mode == GzMode::GZ_READ {
        if state.direct {
            unsafe { free_state(state) };
            return ptr::null_mut();
        }
        state.direct = true; 
    }

    match source {
        Source::Fd(fd) => {
            state.fd = fd;
            state.source = Source::Fd(fd);
        }
        Source::Path(path) => {
            let cloned_path = unsafe { gz_strdup(path) };
            if cloned_path.is_null() {
                unsafe { free_state(state) };
                return ptr::null_mut();
            }
            state.source = Source::Path(cloned_path);
            let mut oflag = 0;

{
                oflag |= libc::O_LARGEFILE;
            }
#[cfg(any())]









            {
                oflag |= libc::O_BINARY;
            }
            if cloexec {
{
                    oflag |= libc::O_CLOEXEC;
                }
            }

            if state.mode == GzMode::GZ_READ {
                oflag |= O_RDONLY;
            } else {
                oflag |= O_WRONLY | O_CREAT;
                if exclusive {
                    oflag |= O_EXCL;
                }
                if state.mode == GzMode::GZ_WRITE {
                    oflag |= O_TRUNC;
                } else {
                    oflag |= O_APPEND;
                }
            }
            state.fd = unsafe { libc::open(cloned_path, oflag, 0o666) };
        }
    }

    if state.fd == -1 {
        unsafe { free_state(state) };
        return ptr::null_mut();
    }

    if state.mode == GzMode::GZ_APPEND {
        lseek64(state.fd, 0, SEEK_END); 
        state.mode = GzMode::GZ_WRITE; 
    }

    if state.mode == GzMode::GZ_READ {
        state.start = lseek64(state.fd, 0, SEEK_CUR) as _;
        if state.start == -1 {
            state.start = 0;
        }
    }

    gz_reset(state);

    (state as *mut GzState).cast::<gzFile_s>()
}

fn fd_path(buf: &mut [u8; 27], fd: c_int) -> &CStr {

    use core::fmt::Write;

    struct Writer<'a> {
        buf: &'a mut [u8; 27],
        len: usize,
    }

    impl Write for Writer<'_> {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            let Some(dst) = self.buf.get_mut(self.len..self.len + s.len()) else {
                return Err(core::fmt::Error);
            };

            dst.copy_from_slice(s.as_bytes());
            self.len += s.len();

            Ok(())
        }
    }

    let mut w = Writer { buf, len: 0 };

    write!(w, "<fd:{fd}>\0").unwrap();

    unsafe { CStr::from_ptr(w.buf[..w.len].as_ptr().cast()) }
}

fn gz_reset(state: &mut GzState) {
    state.have = 0; 
    if state.mode == GzMode::GZ_READ {
        state.eof = false; 
        state.past = false; 
        state.how = How::Look; 
    } else {
        state.reset = false; 
    }
    state.seek = false; 
    unsafe { gz_error(state, None) }; 
    state.pos = 0; 
    state.stream.avail_in = 0; 
}

unsafe fn gz_error(state: &mut GzState, err_msg: Option<(c_int, &str)>) {
    if !state.msg.is_null() {
        unsafe { deallocate_cstr(state.msg.cast_mut()) };
        state.msg = ptr::null_mut();
    }

    match err_msg {
        None => {
            state.err = Z_OK;
        }
        Some((err, msg)) => {
            if err != Z_OK && err != Z_BUF_ERROR {
                state.have = 0;
            }

            state.err = err;

            if err == Z_MEM_ERROR {
                return;
            }

            let sep = ": ";
            let buf = &mut [0u8; 27];
            state.msg = match state.source {
                Source::Path(path) => unsafe {
                    gz_strcat(&[CStr::from_ptr(path).to_str().unwrap(), sep, msg])
                },
                Source::Fd(fd) => unsafe {
                    gz_strcat(&[fd_path(buf, fd).to_str().unwrap(), sep, msg])
                },
            };

            if state.msg.is_null() {
                state.err = Z_MEM_ERROR;
            }
        }
    }
}

unsafe fn free_state(state: *mut GzState) {
    if state.is_null() {
        return;
    }
    unsafe {
        match (*state).source {
            Source::Path(path) => deallocate_cstr(path.cast_mut()),
            Source::Fd(_) => {  }
        }
        deallocate_cstr((*state).msg.cast_mut());
    }
    unsafe { free_buffers(state.as_mut().unwrap()) };

    unsafe { ALLOCATOR.deallocate(state, 1) };
}

unsafe fn free_buffers(state: &mut GzState) {
    if !state.input.is_null() {
        unsafe { ALLOCATOR.deallocate(state.input, state.in_capacity()) };
        state.input = ptr::null_mut();
    }
    state.in_size = 0;
    if !state.output.is_null() {
        unsafe { ALLOCATOR.deallocate(state.output, state.out_capacity()) };
        state.output = ptr::null_mut();
    }
    state.out_size = 0;
}

unsafe fn deallocate_cstr(s: *mut c_char) {
    if s.is_null() {
        return;
    }
    unsafe { ALLOCATOR.deallocate::<c_char>(s, libc::strlen(s) + 1) };
}

/// Close an open gzip file and free the internal data structures referenced by the file handle.
///
/// # Returns
///
/// * [`Z_ERRNO`] if closing the file failed
/// * [`Z_OK`] otherwise
///
/// # Safety
///
/// `file` must be one of the following:
/// - A file handle must have been obtained from a function in this library, such as [`gzopen`].
/// - A null pointer.
///
/// This function may be called at most once for any file handle.
///
/// `file` must not be used after this call returns, as the memory it references may have
/// been deallocated.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzclose))]
pub unsafe extern "C" fn gzclose(file: gzFile) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_ref() }) else {
        return Z_STREAM_ERROR;
    };

    match state.mode {
        GzMode::GZ_READ => unsafe { gzclose_r(file) },
        GzMode::GZ_WRITE | GzMode::GZ_APPEND | GzMode::GZ_NONE => unsafe { gzclose_w(file) },
    }
}

/// Close a gzip file that was opened for reading.
///
/// # Returns
///
/// * Z_OK if `state` has no outstanding error and the file is closed successfully.
/// * A Z_ error code if the `state` is null or the file close operation fails.
///
/// # Safety
///
/// `file` must be one of the following:
/// - A file handle must have been obtained from a function in this library, such as [`gzopen`].
/// - A null pointer.
///
/// `file` must not be used after this call returns, as the memory it references may have
/// been deallocated.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzclose_r))]
pub unsafe extern "C" fn gzclose_r(file: gzFile) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return Z_STREAM_ERROR;
    };

    if state.mode != GzMode::GZ_READ {
        return Z_STREAM_ERROR;
    }

    if state.in_size != 0 {
        unsafe { inflateEnd(&mut state.stream as *mut z_stream) };
    }

    let err = match state.err {
        Z_BUF_ERROR => Z_BUF_ERROR,
        _ => Z_OK,
    };

    let ret = match unsafe { libc::close(state.fd) } {
        0 => err,
        _ => Z_ERRNO,
    };

    unsafe { free_state(file.cast::<GzState>()) };

    ret
}

/// Close a gzip file that was opened for writing.
///
/// # Returns
///
/// * Z_OK if `state` has no outstanding error and the file is closed successfully.
/// * A Z_ error code if the `state` is null or the file close operation fails.
///
/// # Safety
///
/// `file` must be one of the following:
/// - A file handle must have been obtained from a function in this library, such as [`gzopen`].
/// - A null pointer.
///
/// `file` must not be used after this call returns, as the memory it references may have
/// been deallocated.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzclose_w))]
pub unsafe extern "C" fn gzclose_w(file: gzFile) -> c_int {
    let mut ret = Z_OK;

    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return Z_STREAM_ERROR;
    };

    if state.mode != GzMode::GZ_WRITE {
        return Z_STREAM_ERROR;
    }

    if state.seek {
        state.seek = false;
        if gz_zero(state, state.skip as _).is_err() {
            ret = state.err;
        }
    }

    if gz_comp(state, Z_FINISH).is_err() {
        ret = state.err;
    }
    if state.in_size != 0 && !state.direct {
        unsafe { deflateEnd(&mut state.stream as *mut z_stream) };
    }
    if unsafe { libc::close(state.fd) } == -1 {
        ret = Z_ERRNO;
    }

    unsafe { free_state(file.cast::<GzState>()) };

    ret
}

/// Set the internal buffer size used by this library's functions for `file` to
/// `size`.  The default buffer size is 128 KB.  This function must be called
/// after [`gzopen`] or [`gzdopen`], but before any other calls that read or write
/// the file (including [`gzdirect`]).  The buffer memory allocation is always
/// deferred to the first read or write.  Three times `size` in buffer space is
/// allocated.
///
/// # Returns
///
/// * `0` on success.
/// * `-1` on failure.
///
/// # Arguments
///
/// * `file` - file handle.
/// * `size` - requested buffer size in bytes.
///
/// # Safety
///
/// `file` must be one of the following:
/// - A file handle must have been obtained from a function in this library, such as [`gzopen`].
/// - A null pointer.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzbuffer))]
pub unsafe extern "C" fn gzbuffer(file: gzFile, size: c_uint) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return -1;
    };
    if state.mode != GzMode::GZ_READ && state.mode != GzMode::GZ_WRITE {
        return -1;
    }

    if state.in_size != 0 {
        return -1;
    }

    let size = size as usize;
    if size.checked_mul(2).is_none() {
        return -1;
    }

    state.want = Ord::max(size, 8);

    0
}

/// Retrieve the zlib error code and a human-readable string description of
/// the most recent error on a gzip file stream.
///
/// # Arguments
///
/// * `file` - A gzip file handle, or null
/// * `errnum` - A pointer to a C integer in which the zlib error code should be
///   written, or null if the caller does not need the numeric error code.
///
/// # Returns
///
/// * A pointer to a null-terminated C string describing the error, if `file` is non-null
///   and has an error
/// * A pointer to an empty (zero-length), null-terminated C string, if `file` is non-null
///   but has no error
/// * Null otherwise
///
/// # Safety
///
/// `file` must be one of the following:
/// - A file handle obtained from [`gzopen`] or [`gzdopen`].
/// - A null pointer.
///
/// If this function returns a non-null string, the caller must not modifiy or
/// deallocate the string.
///
/// If `errnum` is non-null, it must point to an address at which a [`c_int`] may be written.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzerror))]
pub unsafe extern "C" fn gzerror(file: gzFile, errnum: *mut c_int) -> *const c_char {
    let Some(state) = (unsafe { file.cast::<GzState>().as_ref() }) else {
        return ptr::null();
    };
    if state.mode != GzMode::GZ_READ && state.mode != GzMode::GZ_WRITE {
        return ptr::null();
    }

    if !errnum.is_null() {
        unsafe { *errnum = state.err };
    }
    if state.err == Z_MEM_ERROR {
        b"out of memory\0".as_ptr().cast::<c_char>()
    } else if state.msg.is_null() {
        b"\0".as_ptr().cast::<c_char>()
    } else {
        state.msg
    }
}

/// Clear the error and end-of-file state for `file`.
///
/// # Arguments
///
/// * `file` - A gzip file handle, or null
///
/// # Safety
///
/// `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzclearerr))]
pub unsafe extern "C" fn gzclearerr(file: gzFile) {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return;
    };
    if state.mode != GzMode::GZ_READ && state.mode != GzMode::GZ_WRITE {
        return;
    }

    if state.mode == GzMode::GZ_READ {
        state.eof = false;
        state.past = false;
    }

    unsafe { gz_error(state, None) };
}

/// Check whether a read operation has tried to read beyond the end of `file`.
///
/// # Returns
///
/// * 1 if the end-of-file indicator is set. Note that this indicator is set only
///   if a read tries to go past the end of the input. If the last read request
///   attempted to read exactly the number of bytes remaining in the file, the
///   end-of-file indicator will not be set.
/// * 0 the end-of-file indicator is not set or `file` is null
///
/// # Arguments
///
/// * `file` - A gzip file handle, or null
///
/// # Safety
///
/// `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzeof))]
pub unsafe extern "C" fn gzeof(file: gzFile) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_ref() }) else {
        return 0;
    };
    if state.mode != GzMode::GZ_READ {
        return 0;
    }

    state.past as _
}

/// Check whether `file` is in direct mode (reading or writing literal bytes without compression).
///
/// NOTE: If `gzdirect` is called immediately after [`gzopen`] or [`gzdopen`], it may allocate
/// buffers internally to read the file header and determine whether the content is a gzip file.
/// If [`gzbuffer`] is used, it should be called before `gzdirect`.
///
/// # Returns
///
/// 0 if `file` is null.
///
/// If `file` is being read,
/// * 1 if the contents are being read directly, without decompression.
/// * 0 if the contents are being decompressed when read.
///
/// If `file` is being written,
/// * 1 if transparent mode was requested upon open (with the `"wT"` mode flag for [`gzopen`]).
/// * 0 otherwise.
///
/// # Arguments
///
/// * `file` - A gzip file handle, or null
///
/// # Safety
///
/// `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzdirect))]
pub unsafe extern "C" fn gzdirect(file: gzFile) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return 0;
    };

    if state.mode == GzMode::GZ_READ && state.how == How::Look && state.have == 0 {
        let _ = unsafe { gz_look(state) };
    }

    state.direct as _
}

/// Read and decompress up to `len` uncompressed bytes from `file` into `buf`.  If
/// the input file is not in gzip format, `gzread` copies up to `len` bytes into
/// the buffer directly from the file.
///
/// After reaching the end of a gzip stream in the input, `gzread` will continue
/// to read, looking for another gzip stream.  Any number of gzip streams may be
/// concatenated in the input file, and will all be decompressed by `gzread()`.
/// If something other than a gzip stream is encountered after a gzip stream,
/// `gzread` ignores that remaining trailing garbage (and no error is returned).
///
/// `gzread` can be used to read a gzip file that is being concurrently written.
/// Upon reaching the end of the input, `gzread` will return with the available
/// data.  If the error code returned by [`gzerror`] is `Z_OK` or `Z_BUF_ERROR`,
/// then [`gzclearerr`] can be used to clear the end of file indicator in order
/// to permit `gzread` to be tried again.  `Z_OK` indicates that a gzip stream
/// was completed on the last `gzread`.  `Z_BUF_ERROR` indicates that the input
/// file ended in the middle of a gzip stream.  Note that `gzread` does not return
/// `-1` in the event of an incomplete gzip stream.  This error is deferred until
/// [`gzclose`], which will return `Z_BUF_ERROR` if the last gzread ended in the
/// middle of a gzip stream.  Alternatively, `gzerror` can be used before `gzclose`
/// to detect this case.
///
/// If the unsigned value `len` is too large to fit in the signed return type
/// `c_int`, then nothing is read, `-1` is returned, and the error state is set to
/// `Z_STREAM_ERROR`.
///
/// # Returns
///
/// * The number of uncompressed bytes read from the file into `buf`, which may
///   be smaller than `len` if there is insufficient data in the file.
/// * `-1` on error.
///
/// # Arguments
///
/// * `file` - A gzip file handle, or null.
/// * `buf` - Buffer where the read data should be stored. The caller retains ownership of this buffer.
/// * `len` - Number of bytes to attempt to read into `buf`.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
/// - The caller must ensure that `buf` points to at least `len` writable bytes.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzread))]
pub unsafe extern "C" fn gzread(file: gzFile, buf: *mut c_void, len: c_uint) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return -1;
    };

    if state.mode != GzMode::GZ_READ || (state.err != Z_OK && state.err != Z_BUF_ERROR) {
        return -1;
    }

    if c_int::try_from(len).is_err() {
        const MSG: &str = "request does not fit in an int";
        unsafe { gz_error(state, Some((Z_STREAM_ERROR, MSG))) };
        return -1;
    }

    let got = unsafe { gz_read(state, buf.cast::<u8>(), len as usize) };

    if got == 0 && state.err != Z_OK && state.err != Z_BUF_ERROR {
        -1
    } else {
        got as _
    }
}

/// Read and decompress up to `nitems` items of size `size` from `file` into `buf`,
/// otherwise operating as [`gzread`] does. This duplicates the interface of
/// C stdio's `fread()`, with `size_t` request and return types.
///
/// `gzfread` returns the number of full items read of size `size`, or zero if
/// the end of the file was reached and a full item could not be read, or if
/// there was an error.  [`gzerror`] must be consulted if zero is returned in
/// order to determine if there was an error.  If the multiplication of `size` and
/// `nitems` overflows, i.e. the product does not fit in a `size_t`, then nothing
/// is read, zero is returned, and the error state is set to `Z_STREAM_ERROR`.
///
/// In the event that the end of file is reached and only a partial item is
/// available at the end, i.e. the remaining uncompressed data length is not a
/// multiple of `size`, then the final partial item is nevertheless read into `buf`
/// and the end-of-file flag is set.  The length of the partial item read is not
/// provided, but could be inferred from the result of [`gztell`].  This behavior
/// is the same as the behavior of `fread` implementations in common libraries,
/// but it prevents the direct use of `gzfread` to read a concurrently written
/// file, resetting and retrying on end-of-file, when `size` is not 1.
///
/// # Returns
///
/// - The number of complete object of size `size` read into `buf`.
/// - `0` on error or end-of-file.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
/// - The caller must ensure that `buf` points to at least `size * nitems` writable bytes.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzfread))]
pub unsafe extern "C" fn gzfread(
    buf: *mut c_void,
    size: size_t,
    nitems: size_t,
    file: gzFile,
) -> size_t {
    if size == 0 || buf.is_null() {
        return 0;
    }

    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return 0;
    };

    if state.mode != GzMode::GZ_READ || (state.err != Z_OK && state.err != Z_BUF_ERROR) {
        return 0;
    }

    let Some(len) = size.checked_mul(nitems) else {
        const MSG: &str = "request does not fit in a size_t";
        unsafe { gz_error(state, Some((Z_STREAM_ERROR, MSG))) };
        return 0;
    };

    if len == 0 {
        len
    } else {
        (unsafe { gz_read(state, buf.cast::<u8>(), len) }) / size
    }
}

unsafe fn gz_read(state: &mut GzState, mut buf: *mut u8, mut len: usize) -> usize {
    if len == 0 {
        return 0;
    }

    if state.seek {
        state.seek = false;
        if gz_skip(state, state.skip).is_err() {
            return 0;
        }
    }

    let mut got = 0;
    loop {
        let mut n = Ord::min(len, c_uint::MAX as usize);

        if state.have != 0 {
            n = Ord::min(n, state.have as usize);
            unsafe { ptr::copy_nonoverlapping(state.next, buf, n) };
            state.next = unsafe { state.next.add(n) };
            state.have -= n as c_uint;
        } else if state.eof && state.stream.avail_in == 0 {
            state.past = true; 
            break;
        } else if state.how == How::Look || n < state.in_size * 2 {
            if unsafe { gz_fetch(state) }.is_err() {
                return 0;
            }

            continue;
        } else if state.how == How::Copy {
            let Ok(bytes_read) = (unsafe { gz_load(state, buf, n) }) else {
                return 0;
            };
            n = bytes_read;
        } else {
            debug_assert_eq!(state.how, How::Gzip);
            state.stream.avail_out = n as c_uint;
            state.stream.next_out = buf;
            if unsafe { gz_decomp(state) }.is_err() {
                return 0;
            }
            n = state.have as usize;
            state.have = 0;
        }

        len -= n;
        buf = unsafe { buf.add(n) };
        got += n;
        state.pos += n as i64;

        if len == 0 {
            break;
        }
    }

    got
}

macro_rules! gt_off {
    ($x:expr) => {
        core::mem::size_of_val(&$x) == core::mem::size_of::<i64>()
            && $x as usize > i64::MAX as usize
    };
}

fn gz_skip(state: &mut GzState, mut len: i64) -> Result<(), ()> {
    while len != 0 {
        if state.have != 0 {
            let n = if gt_off!(state.have) || state.have as i64 > len {
                len as usize
            } else {
                state.have as usize
            };
            state.have -= n as c_uint;
            state.next = unsafe { state.next.add(n) };
            state.pos += n as i64;
            len -= n as i64;
        } else if state.eof && state.stream.avail_in == 0 {
            break;
        } else {
            if unsafe { gz_fetch(state) }.is_err() {
                return Err(());
            }
        }
    }
    Ok(())
}

unsafe fn gz_look(state: &mut GzState) -> Result<(), ()> {
    if state.input.is_null() {
        let capacity = state.in_capacity();
        state.in_size = capacity;
        let Some(input) = ALLOCATOR.allocate_slice_raw::<u8>(capacity) else {
            unsafe { gz_error(state, Some((Z_MEM_ERROR, "out of memory"))) };
            return Err(());
        };
        state.input = input.as_ptr();

        if state.output.is_null() {
            let capacity = state.out_capacity();
            state.out_size = capacity;
            let Some(output) = ALLOCATOR.allocate_slice_raw::<u8>(capacity) else {
                unsafe { free_buffers(state) };
                unsafe { gz_error(state, Some((Z_MEM_ERROR, "out of memory"))) };
                return Err(());
            };
            state.output = output.as_ptr();
        }

        state.stream.avail_in = 0;
        state.stream.next_in = ptr::null_mut();
        if unsafe {
            inflateInit2_(
                &mut state.stream as *mut z_stream,
                MAX_WBITS + 16,
                zlibVersion(),
                core::mem::size_of::<z_stream>() as i32,
            )
        } != Z_OK
        {
            unsafe { free_buffers(state) };
            unsafe { gz_error(state, Some((Z_MEM_ERROR, "out of memory"))) };
            return Err(());
        }
    }

    if state.stream.avail_in < 2 {
        if unsafe { gz_avail(state) }? == 0 {
            return Ok(());
        }
    }

    if state.stream.avail_in > 1
        && unsafe { *state.stream.next_in } == 31
        && unsafe { *state.stream.next_in.add(1) } == 139
    {
        unsafe { inflateReset(&mut state.stream as *mut z_stream) };
        state.how = How::Gzip;
        state.direct = false;
        return Ok(());
    }

    if !state.direct {
        state.stream.avail_in = 0;
        state.eof = true;
        state.have = 0;
        return Ok(());
    }

    unsafe {
        ptr::copy_nonoverlapping(
            state.stream.next_in,
            state.output,
            state.stream.avail_in as usize,
        )
    };
    state.next = state.output;
    state.have = state.stream.avail_in;
    state.stream.avail_in = 0;
    state.how = How::Copy;
    state.direct = true;

    Ok(())
}

unsafe fn gz_avail(state: &mut GzState) -> Result<usize, ()> {
    if state.err != Z_OK && state.err != Z_BUF_ERROR {
        return Err(());
    }
    if !state.eof {
        if state.stream.avail_in != 0 {
            unsafe {
                ptr::copy(
                    state.stream.next_in,
                    state.input,
                    state.stream.avail_in as usize,
                )
            };
        }
        let got = unsafe {
            gz_load(
                state,
                state.input.add(state.stream.avail_in as usize),
                state.in_size - state.stream.avail_in as usize,
            )
        }?;
        state.stream.avail_in += got as uInt;
        state.stream.next_in = state.input;
    }
    Ok(state.stream.avail_in as usize)
}

unsafe fn gz_load(state: &mut GzState, buf: *mut u8, len: usize) -> Result<usize, ()> {
    let mut have = 0;
    let mut ret = 0;
    while have < len {
        ret = unsafe { libc::read(state.fd, buf.add(have).cast::<_>(), (len - have) as _) };
        if ret <= 0 {
            break;
        }
        have += ret as usize;
    }
    if ret < 0 {
        unsafe { gz_error(state, Some((Z_ERRNO, "read error"))) }; 
        return Err(());
    }
    if ret == 0 {
        state.eof = true;
    }
    Ok(have)
}

unsafe fn gz_fetch(state: &mut GzState) -> Result<(), ()> {
    loop {
        match &state.how {
            How::Look => {
                unsafe { gz_look(state) }?;
                if state.how == How::Look {
                    return Ok(());
                }
            }
            How::Copy => {
                let bytes_read = unsafe { gz_load(state, state.output, state.out_size) }?;
                state.next = state.output;
                state.have += bytes_read as uInt;
                return Ok(());
            }
            How::Gzip => {
                state.stream.avail_out = state.out_size as c_uint;
                state.stream.next_out = state.output;
                unsafe { gz_decomp(state) }?;
            }
        }

        if state.have != 0 || (state.eof && state.stream.avail_in == 0) {
            break;
        }
    }

    Ok(())
}

unsafe fn gz_decomp(state: &mut GzState) -> Result<(), ()> {
    let had = state.stream.avail_out;
    loop {
        if state.stream.avail_in == 0 && unsafe { gz_avail(state) }.is_err() {
            return Err(());
        }
        if state.stream.avail_in == 0 {
            unsafe { gz_error(state, Some((Z_BUF_ERROR, "unexpected end of file"))) };
            break;
        }

        match unsafe { inflate(&mut state.stream, Z_NO_FLUSH) } {
            Z_STREAM_ERROR | Z_NEED_DICT => {
                const MSG: &str = "internal error: inflate stream corrupt";
                unsafe { gz_error(state, Some((Z_STREAM_ERROR, MSG))) };
                return Err(());
            }
            Z_MEM_ERROR => {
                unsafe { gz_error(state, Some((Z_MEM_ERROR, "out of memory"))) };
                return Err(());
            }
            Z_DATA_ERROR => {
                unsafe { gz_error(state, Some((Z_DATA_ERROR, "compressed data error"))) };
                return Err(());
            }
            Z_STREAM_END => {
                state.how = How::Look;
                break;
            }
            _ => {}
        }

        if state.stream.avail_out == 0 {
            break;
        }
    }

    state.have = had - state.stream.avail_out;
    state.next = unsafe { state.stream.next_out.sub(state.have as usize) };

    Ok(())
}

/// Compress and write the len uncompressed bytes at buf to file.
///
/// # Returns
///
/// - The number of uncompressed bytes written, on success.
/// - Or 0 in case of error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
/// - `buf` must point to at least `len` bytes of readable memory.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzwrite))]
pub unsafe extern "C" fn gzwrite(file: gzFile, buf: *const c_void, len: c_uint) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return 0;
    };

    if state.mode != GzMode::GZ_WRITE || state.err != Z_OK {
        return 0;
    }

    if c_int::try_from(len).is_err() {
        const MSG: &str = "requested length does not fit in int";
        unsafe { gz_error(state, Some((Z_DATA_ERROR, MSG))) };
        return 0;
    }

    let Ok(len) = usize::try_from(len) else {
        const MSG: &str = "requested length does not fit in usize";
        unsafe { gz_error(state, Some((Z_DATA_ERROR, MSG))) };
        return 0;
    };

    unsafe { gz_write(state, buf, len) }
}

/// Compress and write `nitems` items of size `size` from `buf` to `file`, duplicating
/// the interface of C stdio's `fwrite`, with `size_t` request and return types.
///
/// # Returns
///
/// - The number of full items written of size `size` on success.
/// - Zero on error.
///
/// Note: If the multiplication of `size` and `nitems` overflows, i.e. the product does
/// not fit in a `size_t`, then nothing is written, zero is returned, and the error state
/// is set to `Z_STREAM_ERROR`.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
/// - The caller must ensure that `buf` points to at least `size * nitems` readable bytes.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzfwrite))]
pub unsafe extern "C" fn gzfwrite(
    buf: *const c_void,
    size: size_t,
    nitems: size_t,
    file: gzFile,
) -> size_t {
    if size == 0 || buf.is_null() {
        return 0;
    }

    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return 0;
    };

    if state.mode != GzMode::GZ_WRITE || state.err != Z_OK {
        return 0;
    }

    let Some(len) = size.checked_mul(nitems) else {
        const MSG: &str = "request does not fit in a size_t";
        unsafe { gz_error(state, Some((Z_STREAM_ERROR, MSG))) };
        return 0;
    };

    if len == 0 {
        len
    } else {
        (unsafe { gz_write(state, buf, len) }) as size_t / size
    }
}

/// - `state` must have been properly initialized, e.g. by [`gzopen_help`].
/// - `buf` must point to at least `len` bytes of readable memory.
unsafe fn gz_write(state: &mut GzState, mut buf: *const c_void, mut len: usize) -> c_int {
    if len == 0 {
        return 0;
    }

    if state.input.is_null() && gz_init(state).is_err() {
        return 0;
    }

    if state.seek {
        state.seek = false;
        if gz_zero(state, state.skip as _).is_err() {
            return 0;
        }
    }

    let put = len as c_int;

    if len < state.in_size {
        loop {
            if state.stream.avail_in == 0 {
                state.stream.next_in = state.input;
            }
            let have = unsafe { state.input_len() };
            let copy = Ord::min(state.in_size.saturating_sub(have), len);
            unsafe { ptr::copy(buf, state.input.add(have).cast::<c_void>(), copy) };
            state.stream.avail_in += copy as c_uint;
            state.pos += copy as i64;
            buf = unsafe { buf.add(copy) };
            len -= copy;
            if len != 0 && gz_comp(state, Z_NO_FLUSH).is_err() {
                return 0;
            }
            if len == 0 {
                break;
            }
        }
    } else {
        if state.stream.avail_in != 0 && gz_comp(state, Z_NO_FLUSH).is_err() {
            return 0;
        }

        let save_next_in = state.stream.next_in;
        state.stream.next_in = buf.cast::<_>();
        loop {
            let n = Ord::min(len, c_uint::MAX as usize) as c_uint;
            state.stream.avail_in = n;
            state.pos += n as i64;
            if gz_comp(state, Z_NO_FLUSH).is_err() {
                return 0;
            }
            len -= n as usize;
            if len == 0 {
                break;
            }
        }
        state.stream.next_in = save_next_in;
    }

    put
}

fn gz_zero(state: &mut GzState, mut len: usize) -> Result<(), ()> {
    if state.stream.avail_in != 0 && gz_comp(state, Z_NO_FLUSH).is_err() {
        return Err(());
    }

    let mut first = true;
    while len != 0 {
        let n = Ord::min(state.in_size, len);
        if first {
            unsafe { state.input.write_bytes(0u8, n) };
            first = false;
        }
        state.stream.avail_in = n as _;
        state.stream.next_in = state.input;
        state.pos += n as i64;
        if gz_comp(state, Z_NO_FLUSH).is_err() {
            return Err(());
        }
        len -= n;
    }

    Ok(())
}

fn gz_init(state: &mut GzState) -> Result<(), ()> {
    let capacity = state.in_capacity();
    state.in_size = capacity / 2;
    let Some(input) = ALLOCATOR.allocate_slice_raw::<u8>(capacity) else {
        unsafe { gz_error(state, Some((Z_MEM_ERROR, "out of memory"))) };
        return Err(());
    };
    state.input = input.as_ptr();

    if !state.direct {
        let capacity = state.out_capacity();
        state.out_size = capacity;
        let Some(output) = ALLOCATOR.allocate_slice_raw::<u8>(capacity) else {
            unsafe { free_buffers(state) };
            unsafe { gz_error(state, Some((Z_MEM_ERROR, "out of memory"))) };
            return Err(());
        };
        state.output = output.as_ptr();

        state.stream.zalloc = Some(ALLOCATOR.zalloc);
        state.stream.zfree = Some(ALLOCATOR.zfree);
        state.stream.opaque = ALLOCATOR.opaque;
        const DEF_MEM_LEVEL: c_int = 8;
        if unsafe {
            deflateInit2_(
                &mut state.stream,
                state.level as _,
                Z_DEFLATED,
                MAX_WBITS + 16,
                DEF_MEM_LEVEL,
                state.strategy as _,
                zlibVersion(),
                core::mem::size_of::<z_stream>() as _,
            )
        } != Z_OK
        {
            unsafe { free_buffers(state) };
            unsafe { gz_error(state, Some((Z_MEM_ERROR, "out of memory"))) };
            return Err(());
        }
        state.stream.next_in = ptr::null_mut();
    }


    if !state.direct {
        state.stream.avail_out = state.out_size as _;
        state.stream.next_out = state.output;
        state.next = state.stream.next_out;
    }

    Ok(())
}

fn gz_comp(state: &mut GzState, flush: c_int) -> Result<(), ()> {
    if state.input.is_null() && gz_init(state).is_err() {
        return Err(());
    }

    if state.direct {
        let got = unsafe {
            libc::write(
                state.fd,
                state.stream.next_in.cast::<c_void>(),
                state.stream.avail_in as _,
            )
        };
        if got < 0 || got as c_uint != state.stream.avail_in {
            unsafe { gz_error(state, Some((Z_ERRNO, "write error"))) };
            return Err(());
        }
        state.stream.avail_in = 0;
        return Ok(());
    }

    if state.reset {
        if state.stream.avail_in == 0 {
            return Ok(());
        }
        let _ = unsafe { deflateReset(&mut state.stream) };
        state.reset = false;
    }

    let mut ret = Z_OK;
    loop {
        if state.stream.avail_out == 0
            || (flush != Z_NO_FLUSH && (flush != Z_FINISH || ret == Z_STREAM_END))
        {
            let have = unsafe { state.stream.next_out.offset_from(state.next) };
            if have < 0 {
                const MSG: &str = "corrupt internal state in gz_comp";
                unsafe { gz_error(state, Some((Z_STREAM_ERROR, MSG))) };
                return Err(());
            }
            if have != 0 {
                let ret = unsafe { libc::write(state.fd, state.next.cast::<c_void>(), have as _) };
                if ret != have as _ {
                    unsafe { gz_error(state, Some((Z_ERRNO, "write error"))) };
                    return Err(());
                }
            }
            if state.stream.avail_out == 0 {
                state.stream.avail_out = state.out_size as _;
                state.stream.next_out = state.output;
            }
            state.next = state.stream.next_out;
        }

        let mut have = state.stream.avail_out;
        ret = unsafe { deflate(&mut state.stream, flush) };
        if ret == Z_STREAM_ERROR {
            const MSG: &str = "internal error: deflate stream corrupt";
            unsafe { gz_error(state, Some((Z_STREAM_ERROR, MSG))) };
            return Err(());
        }
        have -= state.stream.avail_out;

        if have == 0 {
            break;
        }
    }

    if flush == Z_FINISH {
        state.reset = true;
    }

    Ok(())
}

/// Flush all pending output buffered in `file`. The parameter `flush` is interpreted
/// the same way as in the [`deflate`] function. The return value is the zlib error
/// number (see [`gzerror`]). `gzflush` is permitted only when writing.
///
/// # Returns
///
/// - `Z_OK` on success.
/// - a `Z_` error code on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzflush))]
pub unsafe extern "C" fn gzflush(file: gzFile, flush: c_int) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return Z_STREAM_ERROR;
    };

    if state.mode != GzMode::GZ_WRITE || state.err != Z_OK {
        return Z_STREAM_ERROR;
    }

    if !(0..=Z_FINISH).contains(&flush) {
        return Z_STREAM_ERROR;
    }

    if state.seek {
        state.seek = false;
        if gz_zero(state, state.skip as _).is_err() {
            return state.err;
        }
    }

    let _ = gz_comp(state, flush);
    state.err
}

/// Return the starting position for the next [`gzread`] or [`gzwrite`] on `file`.
/// This position represents a number of bytes in the uncompressed data stream,
/// and is zero when starting, even if appending or reading a gzip stream from
/// the middle of a file using [`gzdopen`].
///
/// # Returns
///
/// * The number of bytes prior to the current read or write position in the
///   uncompressed data stream, on success.
/// * -1 on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gztell64))]
pub unsafe extern "C" fn gztell64(file: gzFile) -> z_off64_t {
    let Some(state) = (unsafe { file.cast::<GzState>().as_ref() }) else {
        return -1;
    };

    if state.mode != GzMode::GZ_READ && state.mode != GzMode::GZ_WRITE {
        return -1;
    }

    match state.seek {
        true => (state.pos + state.skip) as z_off64_t,
        false => state.pos as z_off64_t,
    }
}

/// Return the starting position for the next [`gzread`] or [`gzwrite`] on `file`.
/// This position represents a number of bytes in the uncompressed data stream,
/// and is zero when starting, even if appending or reading a gzip stream from
/// the middle of a file using [`gzdopen`].
///
/// # Returns
///
/// * The number of bytes prior to the current read or write position in the
///   uncompressed data stream, on success.
/// * -1 on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gztell))]
pub unsafe extern "C" fn gztell(file: gzFile) -> z_off_t {
    z_off_t::try_from(unsafe { gztell64(file) }).unwrap_or(-1)
}

/// Return the current compressed (actual) read or write offset of `file`.  This
/// offset includes the count of bytes that precede the gzip stream, for example
/// when appending or when using [`gzdopen`] for reading. When reading, the
/// offset does not include as yet unused buffered input. This information can
///
/// # Returns
///
/// * The number of bytes prior to the current read or write position in the
///   compressed data stream, on success.
/// * -1 on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzoffset64))]
pub unsafe extern "C" fn gzoffset64(file: gzFile) -> z_off64_t {
    let Some(state) = (unsafe { file.cast::<GzState>().as_ref() }) else {
        return -1;
    };

    if state.mode != GzMode::GZ_READ && state.mode != GzMode::GZ_WRITE {
        return -1;
    }

    let offset = lseek64(state.fd, 0, SEEK_CUR) as z_off64_t;
    if offset == -1 {
        return -1;
    }

    match state.mode {
        GzMode::GZ_READ => offset - state.stream.avail_in as z_off64_t,
        GzMode::GZ_NONE | GzMode::GZ_WRITE | GzMode::GZ_APPEND => offset,
    }
}

/// Return the current compressed (actual) read or write offset of `file`.  This
/// offset includes the count of bytes that precede the gzip stream, for example
/// when appending or when using [`gzdopen`] for reading. When reading, the
/// offset does not include as yet unused buffered input. This information can
///
/// # Returns
///
/// * The number of bytes prior to the current read or write position in the
///   compressed data stream, on success.
/// * -1 on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzoffset))]
pub unsafe extern "C" fn gzoffset(file: gzFile) -> z_off_t {
    z_off_t::try_from(unsafe { gzoffset64(file) }).unwrap_or(-1)
}

/// Compress and write `c`, converted to an unsigned 8-bit char, into `file`.
///
/// # Returns
///
///  - The value that was written, on success.
///  - `-1` on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzputc))]
pub unsafe extern "C" fn gzputc(file: gzFile, c: c_int) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return -1;
    };

    if state.mode != GzMode::GZ_WRITE || state.err != Z_OK {
        return -1;
    }

    if state.seek {
        state.seek = false;
        if gz_zero(state, state.skip as _).is_err() {
            return -1;
        }
    }

    if !state.input.is_null() {
        if state.stream.avail_in == 0 {
            state.stream.next_in = state.input;
        }
        let have = unsafe { state.input_len() };
        if have < state.in_size {
            unsafe { *state.input.add(have) = c as u8 };
            state.stream.avail_in += 1;
            state.pos += 1;
            return c & 0xff;
        }
    }

    let buf = [c as u8];
    match unsafe { gz_write(state, buf.as_ptr().cast::<c_void>(), 1) } {
        1 => c & 0xff,
        _ => -1,
    }
}

/// Compress and write the given null-terminated string `s` to file, excluding
/// the terminating null character.
///
/// # Returns
///
/// - the number of characters written, on success.
/// - `-1` in case of error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
/// - `s` must point to a null-terminated C string.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzputs))]
pub unsafe extern "C" fn gzputs(file: gzFile, s: *const c_char) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return -1;
    };

    if s.is_null() {
        return -1;
    }

    if state.mode != GzMode::GZ_WRITE || state.err != Z_OK {
        return -1;
    }

    let len = unsafe { libc::strlen(s) };
    if c_int::try_from(len).is_err() {
        const MSG: &str = "string length does not fit in int";
        unsafe { gz_error(state, Some((Z_STREAM_ERROR, MSG))) };
        return -1;
    }
    let put = unsafe { gz_write(state, s.cast::<c_void>(), len) };
    match put.cmp(&(len as i32)) {
        Ordering::Less => -1,
        Ordering::Equal | Ordering::Greater => len as _,
    }
}

/// Read one decompressed byte from `file`.
///
/// Note: The C header file `zlib.h` provides a macro wrapper for `gzgetc` that implements
/// the fast path inline and calls this function for the slow path.
///
/// # Returns
///
/// - The byte read, on success.
/// - `-1` on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzgetc))]
pub unsafe extern "C" fn gzgetc(file: gzFile) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return -1;
    };

    if state.mode != GzMode::GZ_READ || (state.err != Z_OK && state.err != Z_BUF_ERROR) {
        return -1;
    }

    if state.have != 0 {
        state.have -= 1;
        state.pos += 1;
        let ret = unsafe { *state.next };
        state.next = unsafe { state.next.add(1) };
        return c_int::from(ret);
    }

    let mut c = 0u8;
    match unsafe { gz_read(state, core::slice::from_mut(&mut c).as_mut_ptr(), 1) } {
        1 => c_int::from(c),
        _ => -1,
    }
}

/// Backward-compatibility alias for [`gzgetc`].
///
/// # Returns
///
/// - The byte read, on success.
/// - `-1` on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzgetc_))]
pub unsafe extern "C" fn gzgetc_(file: gzFile) -> c_int {
    unsafe { gzgetc(file) }
}

/// Push `c` back onto the stream for file to be read as the first character on
/// the next read.  At least one character of push-back is always allowed.
///
/// `gzungetc` will fail if `c` is `-1`, and may fail if a character has been pushed
/// but not read yet. If `gzungetc` is used immediately after [`gzopen`] or [`gzdopen`],
/// at least the output buffer size of pushed characters is allowed.  (See [`gzbuffer`].)
///
/// The pushed character will be discarded if the stream is repositioned with
/// [`gzseek`] or [`gzrewind`].
///
/// # Returns
///
/// - The character pushed, on success.
/// - `-1` on failure.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzungetc))]
pub unsafe extern "C" fn gzungetc(c: c_int, file: gzFile) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return -1;
    };

    if c < 0 {
        return -1;
    }

    if state.mode != GzMode::GZ_READ || (state.err != Z_OK && state.err != Z_BUF_ERROR) {
        return -1;
    }

    if state.how == How::Look && state.have == 0 {
        let _ = unsafe { gz_look(state) };
    }

    if state.seek {
        state.seek = false;
        if gz_skip(state, state.skip).is_err() {
            return -1;
        }
    }

    if state.have == 0 {
        state.have = 1;
        state.next = unsafe { state.output.add(state.out_size - 1) };
        unsafe { *(state.next as *mut u8) = c as u8 };
        state.pos -= 1;
        state.past = false;
        return c;
    }

    if state.have as usize == state.out_size {
        const MSG: &str = "out of room to push characters";
        unsafe { gz_error(state, Some((Z_DATA_ERROR, MSG))) };
        return -1;
    }

    if state.next == state.output {
        let offset = state.out_size - state.have as usize;

        let dst = unsafe { state.output.add(offset) };

        unsafe { ptr::copy(state.next, dst as _, state.have as _) };
        state.next = dst;
    }
    state.have += 1;
    state.next = unsafe { state.next.sub(1) };
    unsafe { *(state.next as *mut u8) = c as u8 };
    state.pos -= 1;
    state.past = false;
    c
}

/// Read decompressed bytes from `file` into `buf`, until `len-1` characters are
/// read, or until a newline character is read and transferred to `buf`, or an
/// end-of-file condition is encountered.  If any characters are read or if `len`
/// is one, the string is terminated with a null character.  If no characters
/// are read due to an end-of-file or `len` is less than one, then the buffer is
/// left untouched.
///
/// Note: This function generally only makes sense for files where the decompressed
/// content is text. If there are any null bytes, this function will copy them into
/// `buf` just like any other character, resulting in early truncation of the
/// returned C string. To read gzip files whose decompressed content is binary,
/// please see [`gzread`].
///
/// # Returns
///
/// - `buf`, which now is a null-terminated string, on success.
/// - `null` on error. If there was an error, the contents at `buf` are indeterminate.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
/// - `buf` must be null or a pointer to at least `len` writable bytes.
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzgets))]
pub unsafe extern "C" fn gzgets(file: gzFile, buf: *mut c_char, len: c_int) -> *mut c_char {
    if buf.is_null() || len < 1 {
        return ptr::null_mut();
    }

    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return ptr::null_mut();
    };

    if state.mode != GzMode::GZ_READ || (state.err != Z_OK && state.err != Z_BUF_ERROR) {
        return ptr::null_mut();
    }

    if state.seek {
        state.seek = false;
        if gz_skip(state, state.skip).is_err() {
            return ptr::null_mut();
        }
    }

    let mut left = len as usize - 1;
    if left == 0 {
        unsafe { *buf = 0 };
        return buf;
    }
    let mut dst = buf;
    loop {
        if state.have == 0 && unsafe { gz_fetch(state) }.is_err() {
            return ptr::null_mut();
        }
        if state.have == 0 {
            state.past = true;
            break;
        }

        let mut n = Ord::min(left, state.have as _);
        let eol = unsafe { libc::memchr(state.next.cast::<c_void>(), '\n' as c_int, n as _) };
        if !eol.is_null() {
            n = unsafe { eol.cast::<u8>().offset_from(state.next) } as usize + 1;
        }

        unsafe { ptr::copy_nonoverlapping(state.next, dst as _, n) };
        state.have -= n as c_uint;
        state.next = unsafe { state.next.add(n) };
        state.pos += n as i64;
        left -= n;
        dst = unsafe { dst.add(n) };

        if left == 0 || !eol.is_null() {
            break;
        }
    }

    if dst == buf {
        ptr::null_mut()
    } else {
        unsafe { *dst = 0 };
        buf
    }
}

/// Dynamically update the compression level and strategy for `file`. See the
/// description of [`deflateInit2_`] for the meaning of these parameters. Previously
/// provided data is flushed before applying the parameter changes.
///
/// Note: If `level` is not valid, this function will silently fail with a return
/// value of `Z_OK`, matching the semantics of the C zlib version. However, if
/// `strategy` is not valid, this function will return an error.
///
/// # Returns
///
/// - [`Z_OK`] on success.
/// - [`Z_STREAM_ERROR`] if the file was not opened for writing.
/// - [`Z_ERRNO`] if there is an error writing the flushed data.
/// - [`Z_MEM_ERROR`] if there is a memory allocation error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzsetparams))]
pub unsafe extern "C" fn gzsetparams(file: gzFile, level: c_int, strategy: c_int) -> c_int {
    let Ok(strategy) = Strategy::try_from(strategy) else {
        return Z_STREAM_ERROR;
    };
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return Z_STREAM_ERROR;
    };

    if state.mode != GzMode::GZ_WRITE || state.err != Z_OK || state.direct {
        return Z_STREAM_ERROR;
    }

    if level == c_int::from(state.level) && strategy == state.strategy {
        return Z_OK;
    }

    if state.seek {
        state.seek = false;
        if gz_zero(state, state.skip as _).is_err() {
            return state.err;
        }
    }

    if !state.input.is_null() {
        if state.stream.avail_in != 0 && gz_comp(state, Z_BLOCK).is_err() {
            return state.err;
        }
        unsafe { super::deflateParams(&mut state.stream, level, strategy as c_int) };
    }
    state.level = level as _;
    state.strategy = strategy;
    Z_OK
}

/// Set the starting position to `offset` relative to `whence` for the next [`gzread`]
/// or [`gzwrite`] on `file`. The `offset` represents a number of bytes in the
/// uncompressed data stream. The `whence` parameter is defined as in `lseek(2)`,
/// but only `SEEK_CUR` (relative to current position) and `SEEK_SET` (absolute from
/// start of the uncompressed data stream) are supported.
///
/// If `file` is open for reading, this function is emulated but can extremely
/// slow (because it operates on the decompressed data stream).  If `file` is open
/// for writing, only forward seeks are supported; `gzseek` then compresses a sequence
/// of zeroes up to the new starting position. If a negative `offset` is specified in
/// write mode, `gzseek` returns -1.
///
/// # Returns
///
/// - The resulting offset location as measured in bytes from the beginning of the uncompressed
///   stream, on success.
/// - `-1` on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzseek64))]
pub unsafe extern "C" fn gzseek64(file: gzFile, offset: z_off64_t, whence: c_int) -> z_off64_t {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return -1;
    };
    if state.mode != GzMode::GZ_READ && state.mode != GzMode::GZ_WRITE {
        return -1;
    }

    if state.err != Z_OK && state.err != Z_BUF_ERROR {
        return -1;
    }

    if whence != SEEK_SET && whence != SEEK_CUR {
        return -1;
    }

    let mut offset: i64 = offset as _;

    if whence == SEEK_SET {
        offset -= state.pos;
    } else if state.seek {
        offset += state.skip;
    }
    state.seek = false;

    if state.mode == GZ_READ && state.how == How::Copy && state.pos + offset >= 0 {
        let ret = lseek64(
            state.fd,
            offset as z_off64_t - state.have as z_off64_t,
            SEEK_CUR,
        );
        if ret == -1 {
            return -1;
        }
        state.have = 0;
        state.eof = false;
        state.past = false;
        state.seek = false;
        unsafe { gz_error(state, None) };
        state.stream.avail_in = 0;
        state.pos += offset;
        return state.pos as _;
    }

    if offset < 0 {
        if state.mode != GzMode::GZ_READ {
            return -1;
        }
        offset += state.pos;
        if offset < 0 {
            return -1;
        }

        if unsafe { gzrewind_help(state) } == -1 {
            return -1;
        }
    }

    if state.mode == GzMode::GZ_READ {
        let n = if gt_off!(state.have) || state.have as i64 > offset {
            offset as usize
        } else {
            state.have as usize
        };
        state.have -= n as c_uint;
        state.next = unsafe { state.next.add(n) };
        state.pos += n as i64;
        offset -= n as i64;
    }

    if offset != 0 {
        state.seek = true;
        state.skip = offset;
    }

    (state.pos + offset) as _
}

/// Set the starting position to `offset` relative to `whence` for the next [`gzread`]
/// or [`gzwrite`] on `file`. The `offset` represents a number of bytes in the
/// uncompressed data stream. The `whence` parameter is defined as in `lseek(2)`,
/// but only `SEEK_CUR` (relative to current position) and `SEEK_SET` (absolute from
/// start of the uncompressed data stream) are supported.
///
/// If `file` is open for reading, this function is emulated but can extremely
/// slow (because it operates on the decompressed data stream).  If `file` is open
/// for writing, only forward seeks are supported; `gzseek` then compresses a sequence
/// of zeroes up to the new starting position. If a negative `offset` is specified in
/// write mode, `gzseek` returns -1.
///
/// # Returns
///
/// - The resulting offset location as measured in bytes from the beginning of the uncompressed
///   stream, on success.
/// - `-1` on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzseek))]
pub unsafe extern "C" fn gzseek(file: gzFile, offset: z_off_t, whence: c_int) -> z_off_t {
    z_off_t::try_from(unsafe { gzseek64(file, offset as z_off64_t, whence) }).unwrap_or(-1)
}

/// Rewind `file` to the start. This function is supported only for reading.
///
/// Note: `gzrewind(file)` is equivalent to [`gzseek`]`(file, 0, SEEK_SET)`
///
/// # Returns
///
/// - `0` on success.
/// - `-1` on error.
///
/// # Safety
///
/// - `file`, if non-null, must be an open file handle obtained from [`gzopen`] or [`gzdopen`].
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzrewind))]
pub unsafe extern "C" fn gzrewind(file: gzFile) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return -1;
    };

    unsafe { gzrewind_help(state) }
}

unsafe fn gzrewind_help(state: &mut GzState) -> c_int {
    if state.mode != GzMode::GZ_READ || (state.err != Z_OK && state.err != Z_BUF_ERROR) {
        return -1;
    }

    if lseek64(state.fd, state.start as _, SEEK_SET) == -1 {
        return -1;
    }
    gz_reset(state);
    0
}

/// Convert, format, compress, and write the variadic arguments `...` to a file under control of the string format, as in `fprintf`.
///
/// # Returns
///
/// Returns the number of uncompressed bytes actually written, or a negative zlib error code in case of error.
/// The number of uncompressed bytes written is limited to 8191, or one less than the buffer size given to [`gzbuffer`].
/// The caller should assure that this limit is not exceeded. If it is exceeded, then [`gzprintf`] will return `0` with nothing written.
///
/// Contrary to other implementations that can use the insecure `vsprintf`, the `zlib-rs` library always uses `vsnprintf`,
/// so attempting to write more bytes than the limit can never run into buffer overflow issues.
///
/// # Safety
///
/// - The `format`  must be a valid C string
/// - The variadic arguments must correspond with the format string in number and type
#[cfg(feature = "gzprintf")]
#[cfg_attr(docsrs, doc(cfg(feature = "gzprintf")))]
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzprintf))]
pub unsafe extern "C" fn gzprintf(file: gzFile, format: *const c_char, va: ...) -> c_int {
    unsafe { gzvprintf(file, format, va) }
}

/// Convert, format, compress, and write the variable argument list to a file under control of the string format, as in `vfprintf`.
///
/// # Returns
///
/// Returns the number of uncompressed bytes actually written, or a negative zlib error code in case of error.
/// The number of uncompressed bytes written is limited to 8191, or one less than the buffer size given to [`gzbuffer`].
/// The caller should assure that this limit is not exceeded. If it is exceeded, then [`gzvprintf`] will return `0` with nothing written.
///
/// Contrary to other implementations that can use the insecure `vsprintf`, the `zlib-rs` library always uses `vsnprintf`,
/// so attempting to write more bytes than the limit can never run into buffer overflow issues.
///
/// # Safety
///
/// - The `format`  must be a valid C string
/// - The variadic arguments must correspond with the format string in number and type
#[cfg(feature = "gzprintf")]
#[cfg_attr(docsrs, doc(cfg(feature = "gzprintf")))]
#[cfg_attr(feature = "export-symbols", export_name = prefix!(gzvprintf))]
pub unsafe extern "C" fn gzvprintf(
    file: gzFile,
    format: *const c_char,
    va: core::ffi::VaList,
) -> c_int {
    let Some(state) = (unsafe { file.cast::<GzState>().as_mut() }) else {
        return Z_STREAM_ERROR;
    };

    if state.mode != GzMode::GZ_WRITE || state.err != Z_OK {
        return Z_STREAM_ERROR;
    }

    if state.input.is_null() && gz_init(state).is_err() {
        return state.err;
    }

    if state.seek {
        state.seek = false;
        if gz_zero(state, state.skip as _).is_err() {
            return state.err;
        }
    }

    if state.stream.avail_in == 0 {
        state.stream.next_in = state.input;
    }

    let next = unsafe { (state.stream.next_in).add(state.stream.avail_in as usize) }.cast_mut();


    extern "C" {
        fn vsnprintf(
            s: *mut c_char,
            n: libc::size_t,
            format: *const c_char,
            va: core::ffi::VaList,
        ) -> c_int;
    }

    let len = unsafe { vsnprintf(next.cast::<c_char>(), state.in_size, format, va) };

    if len == 0 || len as usize >= state.in_size {
        return 0;
    }

    state.stream.avail_in += len as u32;
    state.pos += i64::from(len);
    if state.stream.avail_in as usize >= state.in_size {
        let left = state.stream.avail_in - state.in_size as u32;
        state.stream.avail_in = state.in_size as u32;
        if gz_comp(state, Z_NO_FLUSH).is_err() {
            return state.err;
        }
        unsafe { core::ptr::copy(state.input.add(state.in_size), state.input, left as usize) };
        state.stream.next_in = state.input;
        state.stream.avail_in = left;
    }

    len
}

unsafe fn gz_strdup(src: *const c_char) -> *mut c_char {
    if src.is_null() {
        return ptr::null_mut();
    }

    let src = unsafe { CStr::from_ptr(src) };

    let len = src.to_bytes_with_nul().len();
    let Some(dst) = ALLOCATOR.allocate_slice_raw::<c_char>(len) else {
        return ptr::null_mut();
    };

    unsafe { core::ptr::copy_nonoverlapping(src.as_ptr(), dst.as_ptr(), len) };

    dst.as_ptr()
}

unsafe fn gz_strcat(strings: &[&str]) -> *mut c_char {
    let mut len = 1; 
    for src in strings {
        len += src.len();
    }
    let Some(buf) = ALLOCATOR.allocate_slice_raw::<c_char>(len) else {
        return ptr::null_mut();
    };
    let start = buf.as_ptr().cast::<c_char>();
    let mut dst = start.cast::<u8>();
    for src in strings {
        let size = src.len();
        unsafe {
            ptr::copy_nonoverlapping(src.as_ptr(), dst, size);
        };
        dst = unsafe { dst.add(size) };
    }
    unsafe { *dst = 0 };
    start
}

fn lseek64(fd: c_int, offset: z_off64_t, origin: c_int) -> z_off64_t {
{
        return unsafe { libc::lseek64(fd, offset as _, origin) as z_off64_t };
    }

    #[allow(unused)]
    {
        (unsafe { libc::lseek(fd, offset as _, origin) }) as z_off64_t
    }
}
