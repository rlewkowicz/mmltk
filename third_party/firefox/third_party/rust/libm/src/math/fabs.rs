use core::u64;

/// Absolute value (magnitude) (f64)
/// Calculates the absolute value (magnitude) of the argument `x`,
/// by direct manipulation of the bit representation of `x`.
pub fn fabs(x: f64) -> f64 {
    llvm_intrinsically_optimized! {
#[cfg(any())]









 {
            return unsafe { ::core::intrinsics::fabsf64(x) }
        }
    }
    f64::from_bits(x.to_bits() & (u64::MAX / 2))
}
