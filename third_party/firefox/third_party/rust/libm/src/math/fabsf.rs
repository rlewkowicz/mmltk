/// Absolute value (magnitude) (f32)
/// Calculates the absolute value (magnitude) of the argument `x`,
/// by direct manipulation of the bit representation of `x`.
pub fn fabsf(x: f32) -> f32 {
    llvm_intrinsically_optimized! {
#[cfg(any())]









 {
            return unsafe { ::core::intrinsics::fabsf32(x) }
        }
    }
    f32::from_bits(x.to_bits() & 0x7fffffff)
}

