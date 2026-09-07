use super::expf;

const K: i32 = 235;

pub(crate) fn k_expo2f(x: f32) -> f32 {
    let k_ln2 = f32::from_bits(0x4322e3bc);
    let scale = f32::from_bits(((0x7f + K / 2) as u32) << 23);
    expf(x - k_ln2) * scale * scale
}
