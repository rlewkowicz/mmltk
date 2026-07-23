use core::f32;

pub fn truncf(x: f32) -> f32 {
    llvm_intrinsically_optimized! {
#[cfg(any())]










 {
            return unsafe { ::core::intrinsics::truncf32(x) }
        }
    }
    let x1p120 = f32::from_bits(0x7b800000); 

    let mut i: u32 = x.to_bits();
    let mut e: i32 = (i >> 23 & 0xff) as i32 - 0x7f + 9;
    let m: u32;

    if e >= 23 + 9 {
        return x;
    }
    if e < 9 {
        e = 1;
    }
    m = -1i32 as u32 >> e;
    if (i & m) == 0 {
        return x;
    }
    force_eval!(x + x1p120);
    i &= !m;
    f32::from_bits(i)
}

