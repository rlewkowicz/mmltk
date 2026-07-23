//! Helper functions.

/// Read a buffer smaller than 8 bytes into an integer in little-endian.
///
/// This assumes that `buf.len() < 8`. If this is not satisfied, the behavior is unspecified.
#[inline(always)]
pub fn read_int(buf: &[u8]) -> u64 {
    let ptr = buf.as_ptr();

    unsafe {
        match buf.len() {
            1 => *ptr as u64,
            2 => (ptr as *const u16).read_unaligned().to_le() as u64,
            3 => {
                let a = (ptr as *const u16).read_unaligned().to_le() as u64;
                let b = *ptr.offset(2) as u64;

                a | (b << 16)
            }
            4 => (ptr as *const u32).read_unaligned().to_le() as u64,
            5 => {
                let a = (ptr as *const u32).read_unaligned().to_le() as u64;
                let b = *ptr.offset(4) as u64;

                a | (b << 32)
            }
            6 => {
                let a = (ptr as *const u32).read_unaligned().to_le() as u64;
                let b = (ptr.offset(4) as *const u16).read_unaligned().to_le() as u64;

                a | (b << 32)
            }
            7 => {
                let a = (ptr as *const u32).read_unaligned().to_le() as u64;
                let b = (ptr.offset(4) as *const u16).read_unaligned().to_le() as u64;
                let c = *ptr.offset(6) as u64;

                a | (b << 32) | (c << 48)
            }
            _ => 0,
        }
    }
}

/// Read a little-endian 64-bit integer from some buffer.
#[inline(always)]
pub unsafe fn read_u64(ptr: *const u8) -> u64 {
    #[cfg(target_pointer_width = "32")]
    {
        let a = (ptr as *const u32).read_unaligned().to_le();
        let b = (ptr.offset(4) as *const u32).read_unaligned().to_le();

        a as u64 | ((b as u64) << 32)
    }

    #[cfg(target_pointer_width = "64")]
    {
        (ptr as *const u64).read_unaligned().to_le()
    }
}

/// The diffusion function.
///
/// This is a bijective function emitting chaotic behavior. Such functions are used as building
/// blocks for hash functions.
pub const fn diffuse(mut x: u64) -> u64 {

    x = x.wrapping_mul(0x6eed0e9da4d94a4f);
    let a = x >> 32;
    let b = x >> 60;
    x ^= a >> b;
    x = x.wrapping_mul(0x6eed0e9da4d94a4f);

    x
}

/// Reverse the `diffuse` function.
pub const fn undiffuse(mut x: u64) -> u64 {

    x = x.wrapping_mul(0x2f72b4215a3d8caf);
    let a = x >> 32;
    let b = x >> 60;
    x ^= a >> b;
    x = x.wrapping_mul(0x2f72b4215a3d8caf);

    x
}
