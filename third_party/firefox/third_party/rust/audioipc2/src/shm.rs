// Copyright © 2017 Mozilla Foundation
// This program is made available under an ISC-style license.  See the
// accompanying file LICENSE for details.

#![allow(clippy::missing_safety_doc)]

use crate::errors::*;
use crate::PlatformHandle;
use std::{convert::TryInto, ffi::c_void, slice};

pub use unix::SharedMem;

#[derive(Copy, Clone)]
struct SharedMemView {
    ptr: *mut c_void,
    size: usize,
}

unsafe impl Send for SharedMemView {}

impl SharedMemView {
    pub unsafe fn get_slice(&self, size: usize) -> Result<&[u8]> {
        let map = slice::from_raw_parts(self.ptr as _, self.size);
        if size <= self.size {
            Ok(&map[..size])
        } else {
            Err(Error::Other("mmap size".into()))
        }
    }

    pub unsafe fn get_mut_slice(&mut self, size: usize) -> Result<&mut [u8]> {
        let map = slice::from_raw_parts_mut(self.ptr as _, self.size);
        if size <= self.size {
            Ok(&mut map[..size])
        } else {
            Err(Error::Other("mmap size".into()))
        }
    }
}

mod unix {
    use super::*;
    use memmap2::{MmapMut, MmapOptions};
    use std::fs::File;
    use std::os::unix::io::{AsRawFd, FromRawFd};


fn open_shm_file(id: &str, size: usize) -> Result<File> {
        let file = open_shm_file_impl(id)?;
        allocate_file(&file, size)?;
        Ok(file)
    }

fn open_shm_file_impl(id: &str) -> Result<File> {
        use std::env::temp_dir;
        use std::fs::{remove_file, OpenOptions};

        let id_cstring = std::ffi::CString::new(id).unwrap();

{
            unsafe {
                let r = libc::syscall(libc::SYS_memfd_create, id_cstring.as_ptr(), 0);
                if r >= 0 {
                    return Ok(File::from_raw_fd(r.try_into().unwrap()));
                }
            }

            let mut path = std::path::PathBuf::from("/dev/shm");
            path.push(id);

            if let Ok(file) = OpenOptions::new()
                .read(true)
                .write(true)
                .create_new(true)
                .open(&path)
            {
                let _ = remove_file(&path);
                return Ok(file);
            }
        }

        unsafe {
            let fd = libc::shm_open(
                id_cstring.as_ptr(),
                libc::O_RDWR | libc::O_CREAT | libc::O_EXCL,
                0o600,
            );
            if fd >= 0 {
                libc::shm_unlink(id_cstring.as_ptr());
                return Ok(File::from_raw_fd(fd));
            }
        }

        let mut path = temp_dir();
        path.push(id);

        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .open(&path)?;

        let _ = remove_file(&path);
        Ok(file)
    }

fn handle_enospc(s: &str) -> Result<()> {
        let err = std::io::Error::last_os_error();
        let errno = err.raw_os_error().unwrap_or(0);
        assert_ne!(errno, 0);
        debug!("allocate_file: {s} failed errno={errno}");
        if errno == libc::ENOSPC {
            return Err(err.into());
        }
        Ok(())
    }

fn allocate_file(file: &File, size: usize) -> Result<()> {

        file.set_len(size.try_into().unwrap())?;

        let fd = file.as_raw_fd();
        let size: libc::off_t = size.try_into().unwrap();

{
            if unsafe { libc::fallocate(fd, 0, 0, size) } == 0 {
                return Ok(());
            }
            handle_enospc("fallocate()")?;
        }

#[cfg(any())]








        {
            let params = libc::fstore_t {
                fst_flags: libc::F_ALLOCATEALL,
                fst_posmode: libc::F_PEOFPOSMODE,
                fst_offset: 0,
                fst_length: size,
                fst_bytesalloc: 0,
            };
            if unsafe { libc::fcntl(fd, libc::F_PREALLOCATE, &params) } == 0 {
                return Ok(());
            }
            handle_enospc("fcntl(F_PREALLOCATE)")?;
        }

{
            if unsafe { libc::posix_fallocate(fd, 0, size) } == 0 {
                return Ok(());
            }
            handle_enospc("posix_fallocate()")?;
        }

        Ok(())
    }

    pub struct SharedMem {
        file: File,
        _mmap: MmapMut,
        view: SharedMemView,
    }

    impl SharedMem {
        pub fn new(id: &str, size: usize) -> Result<SharedMem> {
            let file = open_shm_file(id, size)?;
            let mut mmap = unsafe { MmapOptions::new().len(size).map_mut(&file)? };
            assert_eq!(mmap.len(), size);
            let view = SharedMemView {
                ptr: mmap.as_mut_ptr() as _,
                size,
            };
            Ok(SharedMem {
                file,
                _mmap: mmap,
                view,
            })
        }

        pub unsafe fn make_handle(&self) -> Result<PlatformHandle> {
            PlatformHandle::duplicate(self.file.as_raw_fd()).map_err(|e| e.into())
        }

        pub unsafe fn from(handle: PlatformHandle, size: usize) -> Result<SharedMem> {
            let file = File::from_raw_fd(handle.into_raw());
            let mut mmap = MmapOptions::new().len(size).map_mut(&file)?;
            assert_eq!(mmap.len(), size);
            let view = SharedMemView {
                ptr: mmap.as_mut_ptr() as _,
                size,
            };
            Ok(SharedMem {
                file,
                _mmap: mmap,
                view,
            })
        }

        pub unsafe fn get_slice(&self, size: usize) -> Result<&[u8]> {
            self.view.get_slice(size)
        }

        pub unsafe fn get_mut_slice(&mut self, size: usize) -> Result<&mut [u8]> {
            self.view.get_mut_slice(size)
        }

        pub fn get_size(&self) -> usize {
            self.view.size
        }
    }
}
