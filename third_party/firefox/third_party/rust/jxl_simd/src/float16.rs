// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

//! IEEE 754 half-precision (binary16) floating-point type.
//!
//! This is a minimal implementation providing only the operations needed for JPEG XL decoding,
//! avoiding external dependencies like `half` which pulls in `zerocopy`.

/// IEEE 754 binary16 half-precision floating-point type.
///
/// Format: 1 sign bit, 5 exponent bits (bias 15), 10 mantissa bits.
#[allow(non_camel_case_types)]
#[derive(Copy, Clone, Default, PartialEq, Eq, Hash)]
#[repr(transparent)]
pub struct f16(u16);

impl f16 {
    /// Positive zero.
    pub const ZERO: Self = Self(0);

    /// Creates an f16 from its raw bit representation.
    #[inline]
    pub const fn from_bits(bits: u16) -> Self {
        Self(bits)
    }

    /// Returns the raw bit representation.
    #[inline]
    pub const fn to_bits(self) -> u16 {
        self.0
    }

    /// Converts to f32.
    #[inline]
    pub fn to_f32(self) -> f32 {
        let bits = self.0;
        let sign = ((bits >> 15) & 1) as u32;
        let exp = ((bits >> 10) & 0x1F) as u32;
        let mant = (bits & 0x3FF) as u32;

        let f32_bits = if exp == 0 {
            if mant == 0 {
                sign << 31
            } else {
                let mut m = mant;
                let mut e = 0u32;
                while (m & 0x400) == 0 {
                    m <<= 1;
                    e += 1;
                }
                m &= 0x3FF; 
                let new_exp = 127 - 14 - e;
                (sign << 31) | (new_exp << 23) | (m << 13)
            }
        } else if exp == 31 {
            if mant == 0 {
                (sign << 31) | (0xFF << 23)
            } else {
                (sign << 31) | (0xFF << 23) | (mant << 13) | 0x0040_0000
            }
        } else {
            let new_exp = exp + 112;
            (sign << 31) | (new_exp << 23) | (mant << 13)
        };

        f32::from_bits(f32_bits)
    }

    /// Creates an f16 from an f32.
    #[inline]
    pub fn from_f32(f: f32) -> Self {
        let bits = f.to_bits();
        let sign = ((bits >> 31) & 1) as u16;
        let exp = ((bits >> 23) & 0xFF) as i32;
        let mant = bits & 0x007F_FFFF;

        let h_bits = if exp == 0 {
            sign << 15
        } else if exp == 255 {
            if mant == 0 {
                (sign << 15) | (0x1F << 10) 
            } else {
                (sign << 15) | (0x1F << 10) | 0x0200 
            }
        } else {
            let unbiased = exp - 127;

            if unbiased < -24 {
                sign << 15
            } else if unbiased < -14 {
                let shift = (-14 - unbiased) as u32;
                let m = ((mant | 0x0080_0000) >> (shift + 14)) as u16;
                (sign << 15) | m
            } else if unbiased > 15 {
                (sign << 15) | (0x1F << 10)
            } else {
                let h_exp = (unbiased + 15) as u16;
                let h_mant = (mant >> 13) as u16;

                let round_bit = (mant >> 12) & 1;
                let sticky = mant & 0x0FFF;
                let h_mant = if round_bit == 1 && (sticky != 0 || (h_mant & 1) == 1) {
                    h_mant + 1
                } else {
                    h_mant
                };

                if h_mant > 0x3FF {
                    if h_exp >= 30 {
                        (sign << 15) | (0x1F << 10)
                    } else {
                        (sign << 15) | ((h_exp + 1) << 10)
                    }
                } else {
                    (sign << 15) | (h_exp << 10) | h_mant
                }
            }
        };

        Self(h_bits)
    }

    /// Creates an f16 from an f64.
    #[inline]
    pub fn from_f64(f: f64) -> Self {
        Self::from_f32(f as f32)
    }

    /// Converts to f64.
    #[inline]
    pub fn to_f64(self) -> f64 {
        self.to_f32() as f64
    }

    /// Returns true if this is neither infinite nor NaN.
    #[inline]
    pub fn is_finite(self) -> bool {
        ((self.0 >> 10) & 0x1F) != 31
    }

    /// Returns the bytes in little-endian order.
    #[inline]
    pub const fn to_le_bytes(self) -> [u8; 2] {
        self.0.to_le_bytes()
    }

    /// Returns the bytes in big-endian order.
    #[inline]
    pub const fn to_be_bytes(self) -> [u8; 2] {
        self.0.to_be_bytes()
    }
}

impl From<f16> for f32 {
    #[inline]
    fn from(f: f16) -> f32 {
        f.to_f32()
    }
}

impl From<f16> for f64 {
    #[inline]
    fn from(f: f16) -> f64 {
        f.to_f64()
    }
}

impl core::fmt::Debug for f16 {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{}", self.to_f32())
    }
}

impl core::fmt::Display for f16 {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{}", self.to_f32())
    }
}
