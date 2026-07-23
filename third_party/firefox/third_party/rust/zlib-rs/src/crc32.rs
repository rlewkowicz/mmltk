//! The crc32 checksum algorithm.

use crate::CRC32_INITIAL_VALUE;

#[cfg(target_arch = "aarch64")]
pub(crate) mod acle;
mod braid;
mod combine;
#[cfg(target_arch = "x86_64")]
mod pclmulqdq;
#[cfg(target_arch = "x86_64")]
#[cfg(feature = "vpclmulqdq")]
mod vpclmulqdq;

pub use combine::{crc32_combine, crc32_combine_gen, crc32_combine_op};

pub fn crc32(start: u32, buf: &[u8]) -> u32 {
    if buf.len() < 64 {
        return crc32_braid(start, buf);
    }

    let mut crc_state = Crc32Fold::new_with_initial(start);
    crc_state.fold(buf, start);
    crc_state.finish()
}

fn crc32_braid(start: u32, buf: &[u8]) -> u32 {
    braid::crc32_braid::<5>(start, buf)
}

pub fn get_crc_table() -> &'static [u32; 256] {
    braid::get_crc_table()
}


#[derive(Debug, Clone, Copy)]
pub(crate) struct Crc32Fold {
    #[cfg(target_arch = "x86_64")]
    fold: pclmulqdq::Accumulator,
    value: u32,
}

impl Default for Crc32Fold {
    fn default() -> Self {
        Self::new()
    }
}

impl Crc32Fold {
    pub const fn new() -> Self {
        Self::new_with_initial(CRC32_INITIAL_VALUE)
    }

    pub const fn new_with_initial(initial: u32) -> Self {
        Self {
            #[cfg(target_arch = "x86_64")]
            fold: pclmulqdq::Accumulator::new(),
            value: initial,
        }
    }

    pub fn fold(&mut self, src: &[u8], _start: u32) {
        #[cfg(target_arch = "x86_64")]
        if crate::cpu_features::is_enabled_pclmulqdq() {
            return unsafe { self.fold.fold(src, _start) };
        }

        #[cfg(target_arch = "aarch64")]
        if crate::cpu_features::is_enabled_crc() {
            self.value = unsafe { self::acle::crc32_acle_aarch64(self.value, src) };
            return;
        }

        self.value = braid::crc32_braid::<5>(self.value, src);
    }

    pub fn fold_copy(&mut self, dst: &mut [u8], src: &[u8]) {
        #[cfg(target_arch = "x86_64")]
        if crate::cpu_features::is_enabled_pclmulqdq() {
            return unsafe { self.fold.fold_copy(dst, src) };
        }

        self.fold(src, 0);
        dst[..src.len()].copy_from_slice(src);
    }

    pub fn finish(self) -> u32 {
        #[cfg(target_arch = "x86_64")]
        if crate::cpu_features::is_enabled_pclmulqdq() {
            return unsafe { self.fold.finish() };
        }

        self.value
    }
}
