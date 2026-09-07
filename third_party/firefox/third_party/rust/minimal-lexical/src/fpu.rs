//! Platform-specific, assembly instructions to avoid
//! intermediate rounding on architectures with FPUs.
//!
//! This is adapted from the implementation in the Rust core library,
//! the original implementation can be [here](https://github.com/rust-lang/rust/blob/master/library/core/src/num/dec2flt/fpu.rs).
//!
//! It is therefore also subject to a Apache2.0/MIT license.

#![cfg(feature = "nightly")]
#![doc(hidden)]

pub use fpu_precision::set_precision;

#[cfg(all(target_arch = "x86", not(target_feature = "sse2")))]
mod fpu_precision {
    use core::mem::size_of;

    /// A structure used to preserve the original value of the FPU control word, so that it can be
    /// restored when the structure is dropped.
    ///
    /// The x87 FPU is a 16-bits register whose fields are as follows:
    ///
    /// | 12-15 | 10-11 | 8-9 | 6-7 |  5 |  4 |  3 |  2 |  1 |  0 |
    /// |------:|------:|----:|----:|---:|---:|---:|---:|---:|---:|
    /// |       | RC    | PC  |     | PM | UM | OM | ZM | DM | IM |
    ///
    /// The documentation for all of the fields is available in the IA-32 Architectures Software
    /// Developer's Manual (Volume 1).
    ///
    /// The only field which is relevant for the following code is PC, Precision Control. This
    /// field determines the precision of the operations performed by the  FPU. It can be set to:
    ///  - 0b00, single precision i.e., 32-bits
    ///  - 0b10, double precision i.e., 64-bits
    ///  - 0b11, double extended precision i.e., 80-bits (default state)
    /// The 0b01 value is reserved and should not be used.
    pub struct FPUControlWord(u16);

    fn set_cw(cw: u16) {
        unsafe {
            asm!(
                "fldcw word ptr [{}]",
                in(reg) &cw,
                options(nostack),
            )
        }
    }

    /// Sets the precision field of the FPU to `T` and returns a `FPUControlWord`.
    pub fn set_precision<T>() -> FPUControlWord {
        let mut cw = 0_u16;

        let cw_precision = match size_of::<T>() {
            4 => 0x0000, 
            8 => 0x0200, 
            _ => 0x0300, 
        };

        unsafe {
            asm!(
                "fnstcw word ptr [{}]",
                in(reg) &mut cw,
                options(nostack),
            )
        }

        set_cw((cw & 0xFCFF) | cw_precision);

        FPUControlWord(cw)
    }

    impl Drop for FPUControlWord {
        fn drop(&mut self) {
            set_cw(self.0)
        }
    }
}

#[cfg(any(not(target_arch = "x86"), target_feature = "sse2"))]
mod fpu_precision {
    pub fn set_precision<T>() {
    }
}
